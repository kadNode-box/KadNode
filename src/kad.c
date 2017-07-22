
#define _GNU_SOURCE

#include <sys/time.h>
#include <assert.h>

#include "log.h"
#include "main.h"
#include "utils.h"
#include "conf.h"
#include "utils.h"
#include "net.h"
#include "searches.h"
#include "announces.h"
#ifdef BOB
#include "ext-bob.h"
#endif

#include "dht.c"


/*
* The interface that is used to interact with the DHT.
*/

// Next time to do DHT maintenance
static time_t g_dht_maintenance = 0;
static int g_dht_socket4 = -1;
static int g_dht_socket6 = -1;

void dht_lock_init( void ) {
#ifdef PTHREAD
	pthread_mutex_init( &gconf->dht_mutex, NULL );
#endif
}

void dht_lock( void ) {
#ifdef PTHREAD
	pthread_mutex_lock( &gconf->dht_mutex );
#endif
}

void dht_unlock( void ) {
#ifdef PTHREAD
	pthread_mutex_unlock( &gconf->dht_mutex );
#endif
}

/*
* Put an address and port into a sockaddr_storages struct.
* Both addr and port are in network byte order.
*/
void to_addr( IP *addr, const void *ip, size_t len, uint16_t port ) {
	memset( addr, '\0', sizeof(IP) );

	if( len == 4 ) {
		IP4 *a = (IP4 *) addr;
		a->sin_family = AF_INET;
		a->sin_port = port;
		memcpy( &a->sin_addr.s_addr, ip, 4 );
	}

	if( len == 16 ) {
		IP6 *a = (IP6 *) addr;
		a->sin6_family = AF_INET6;
		a->sin6_port = port;
		memcpy( &a->sin6_addr.s6_addr, ip, 16 );
	}
}

typedef struct {
	uint8_t addr[16];
	uint16_t port;
} dht_addr6_t;

typedef struct {
	uint8_t addr[4];
	uint16_t port;
} dht_addr4_t;


// This callback is called when a search result arrives or a search completes
void dht_callback_func( void *closure, int event, const uint8_t *info_hash, const void *data, size_t data_len ) {
	struct search_t **searches;
	struct search_t *search;
	IP addr;
	size_t i;

	// Find search
	search = NULL;
	searches = searches_get();
	while( *searches ) {
		if( memcmp( (*searches)->id, info_hash, SHA1_BIN_LENGTH ) == 0 ) {
			search = *searches;
			break;
		}
		searches++;
	}

	if( search == NULL ) {
		return;
	}

	switch( event ) {
		case DHT_EVENT_VALUES:
			if( gconf->af == AF_INET ) {
				dht_addr4_t *data4 = (dht_addr4_t *) data;
				for( i = 0; i < (data_len / sizeof(dht_addr4_t)); ++i ) {
					to_addr( &addr, &data4[i].addr, 4, data4[i].port );
					searches_add_addr( search, &addr );
				}
			}
			break;
		case DHT_EVENT_VALUES6:
			if( gconf->af == AF_INET6 ) {
				dht_addr6_t *data6 = (dht_addr6_t *) data;
				for( i = 0; i < (data_len / sizeof(dht_addr6_t)); ++i ) {
					to_addr( &addr, &data6[i].addr, 16, data6[i].port );
					searches_add_addr( search, &addr );
				}
			}
			break;
		case DHT_EVENT_SEARCH_DONE:
		case DHT_EVENT_SEARCH_DONE6:
			// Ignore..
			break;
	}
}

/*
* Lookup in values we announce ourselves.
* Useful for networks of only one node, also faster.
*/
void kad_lookup_local_values( struct search_t *search ) {
	struct value_t* value;
	IP addr;

	// 127.0.0.1
	const uint32_t inaddr_loopback = htonl( INADDR_LOOPBACK );

	value = announces_find( search->id );
	if( value ) {
		if( gconf->af == AF_INET6 ) {
			to_addr( &addr, &in6addr_loopback, 16, htons( value->port ) ); // ::1
		} else {
			to_addr( &addr, &inaddr_loopback, 4, htons( value->port ) ); // 127.0.0.1
		}
		log_debug( "KAD: Address found in own announcements: %s", str_addr( &addr ) );
		searches_add_addr( search, &addr );
	}
}

// Handle incoming packets and pass them to the DHT code
void dht_handler( int rc, int sock ) {
	uint8_t buf[1500];
	uint32_t buflen;
	IP from;
	socklen_t fromlen;
	time_t time_wait = 0;

	if( rc > 0 ) {
		// Check which socket received the data
		fromlen = sizeof(from);
		buflen = recvfrom( sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*) &from, &fromlen );

		if( buflen <= 0 || buflen >= sizeof(buf) ) {
			goto end;
		}

		// The DHT code expects the message to be null-terminated.
		buf[buflen] = '\0';
	} else {
		buflen = 0;
	}

#ifdef BOB
	// Hook up BOB extension on the DHT socket
	if( bob_handler( sock, buf, buflen, &from ) == 0 ) {
		return;
	}
#endif

	if( buflen > 0 ) {
		// Handle incoming data
		dht_lock();
		rc = dht_periodic( buf, buflen, (struct sockaddr*) &from, fromlen, &time_wait, dht_callback_func, NULL );
		dht_unlock();

		if( rc < 0 && errno != EINTR ) {
			if( rc == EINVAL || rc == EFAULT ) {
				log_err( "KAD: Error calling dht_periodic." );
				exit( 1 );
			}
			g_dht_maintenance = time_now_sec() + 1;
		} else {
			g_dht_maintenance = time_now_sec() + time_wait;
		}
	} else if( g_dht_maintenance <= time_now_sec() ) {
		// Do a maintenance call
		dht_lock();
		rc = dht_periodic( NULL, 0, NULL, 0, &time_wait, dht_callback_func, NULL );
		dht_unlock();

		// Wait for the next maintenance call
		g_dht_maintenance = time_now_sec() + time_wait;
		log_debug( "KAD: Next maintenance call in %u seconds.", (unsigned int) time_wait );
	} else {
		rc = 0;
	}

	if( rc < 0 ) {
		if( errno == EINTR ) {
			goto end;
		} else if( rc == EINVAL || rc == EFAULT ) {
			log_err( "KAD: Error using select: %s", strerror( errno ) );
			goto end;
		} else {
			g_dht_maintenance = time_now_sec() + 1;
		}
	}

	end:;
}

/*
* Kademlia needs dht_blacklisted/dht_hash/dht_random_bytes functions to be present.
*/

int dht_blacklisted( const struct sockaddr *sa, int salen ) {
	return 0;
}

// Hashing for the DHT - implementation does not matter for interoperability
void dht_hash( void *hash_return, int hash_size,
		const void *v1, int len1,
		const void *v2, int len2,
		const void *v3, int len3 ) {
	union {
		uint8_t data[8];
		uint16_t num4[4];
		uint32_t num2[2];
		uint64_t num1[1];
	} hash;

	assert( len1 == 8 );
	memcpy( &hash.data, v1, 8);

	assert( len2 == 4 || len2 == 16 );
	if( len2 == 4 ) {
		const uint32_t d2 = *((uint32_t*) v2);
		hash.num2[1] ^= d2;
		hash.num2[2] ^= d2;
	} else {
		hash.num1[1] ^= *((uint64_t*) v2);
		hash.num1[1] ^= *((uint64_t*) v2 + 8);
	}

	assert( len3 == 2 );
	const uint16_t d3 = *((uint16_t*) v3);
	hash.num4[0] ^= d3;
	hash.num4[1] ^= d3;
	hash.num4[2] ^= d3;
	hash.num4[3] ^= d3;

	assert( hash_size == 8 );
	memcpy( hash_return, &hash.data, 8 );
}

int dht_random_bytes( void *buf, size_t size ) {
	return bytes_random( buf, size );
}

void kad_setup( void ) {
	uint8_t node_id[SHA1_BIN_LENGTH];

#ifdef DEBUG
	// Let the DHT output debug text
	dht_debug = stdout;
#endif

	bytes_random( node_id, SHA1_BIN_LENGTH );

	dht_lock_init();

	//gconf->dht_addr
	if( gconf->af == AF_INET ) {
		g_dht_socket4 = net_bind( "KAD", gconf->dht_addr, gconf->dht_port, gconf->dht_ifname, IPPROTO_UDP, AF_INET );
		net_add_handler( g_dht_socket4, &dht_handler );
	} else {
		g_dht_socket6 = net_bind( "KAD", gconf->dht_addr, gconf->dht_port, gconf->dht_ifname, IPPROTO_UDP, AF_INET6 );
		net_add_handler( g_dht_socket6, &dht_handler );
	}

	// Init the DHT.  Also set the sockets into non-blocking mode.
	if( dht_init( g_dht_socket4, g_dht_socket6, node_id, (uint8_t*) "KN\0\0") < 0 ) {
		log_err( "KAD: Failed to initialize the DHT." );
		exit( 1 );
	}
}

void kad_free( void ) {
	dht_uninit();
}

int kad_count_nodes( int good ) {
	struct bucket *bucket;
	struct node *node;
	int count;

	bucket = (gconf->af == AF_INET ) ? buckets : buckets6;
	count = 0;
	while( bucket ) {
		if( good ) {
			node = bucket->nodes;
			while( node ) {
				count += node_good( node ) ? 1 : 0;
				node = node->next;
			}
		} else {
			count += bucket->count;
		}
		bucket = bucket->next;
	}
	return count;
}

#define bprintf(...) (written += snprintf( buf+written, size-written, __VA_ARGS__))

int kad_status( char buf[], size_t size ) {
	struct storage *strg = storage;
	struct search *srch = searches;
	int numsearches_active = 0;
	int numsearches_done = 0;
	int numsearches = 0;
	int numstorage = 0;
	int numstorage_peers = 0;
	//int numvalues = 0;
	int written = 0;
	IP listen_addr;
	socklen_t len;

	// count searches
	while( srch ) {
		if( srch->done ) {
			numsearches_done += 1;
		} else {
			numsearches_active += 1;
		}
		numsearches += 1;
		srch = srch->next;
	}

	// Count storage and peers
	while( strg ) {
		numstorage_peers += strg->numpeers;
		numstorage += 1;
		strg = strg->next;
	}

	// Get address the DHT listens to
	len = sizeof(listen_addr);
	getsockname( g_dht_socket4 < 0 ? g_dht_socket6 : g_dht_socket4, (struct sockaddr *) &listen_addr, &len );

	// Use dht data structure!
	//numvalues = announces_count();

	bprintf( "Version: %s\n", kadnode_version_str );
	bprintf( "DHT id: %s\n", str_id( myid ) );
	bprintf( "DHT listen on: %s / %s\n", str_addr( &listen_addr ),
		(gconf->dht_ifname == NULL) ? "<any>" : gconf->dht_ifname
	);

	bprintf( "DHT Nodes: %d (%d good) (%s)\n",
		kad_count_nodes( 0 ), kad_count_nodes( 1 ), (gconf->af == AF_INET) ? "IPv4" : "IPv6" );
	bprintf( "DHT Storage: %d (max %d), %d peers (max %d per storage)\n",
		numstorage, DHT_MAX_HASHES, numstorage_peers, DHT_MAX_PEERS );
	bprintf( "DHT Searches: %d active, %d completed (max %d)\n",
		numsearches_active, numsearches_done, DHT_MAX_SEARCHES );
	bprintf( "DHT Searches: %d (max %d)\n", numsearches, DHT_MAX_SEARCHES );
	bprintf( "DHT Blacklist: %d (max %d)\n",
		(next_blacklisted % DHT_MAX_BLACKLISTED), DHT_MAX_BLACKLISTED );
	//bprintf( "DHT Values to announce: %d\n", numvalues );

	return written;
}

int kad_ping( const IP* addr ) {
	int rc;

	dht_lock();
	rc = dht_ping_node( (struct sockaddr *)addr, addr_len( addr ) );
	dht_unlock();

	return (rc < 0) ? -1 : 0;
}

/*
* Find nodes that are near the given id and announce to them
* that this node can satisfy the given id on the given port.
*/
int kad_announce_once( const uint8_t id[], int port ) {

	if( port < 1 || port > 65535 ) {
		return -1;
	}

	dht_lock();
	dht_search( id, port, gconf->af, dht_callback_func, NULL );
	dht_unlock();

	return 0;
}

/*
* Add a new value to the announcement list or refresh an announcement.
*/
int kad_announce( const char query_raw[], int port, time_t lifetime ) {
	char query[QUERY_MAX_SIZE];

	// Remove .p2p suffix and convert to lowercase
	if( query_sanitize( query, sizeof(query), query_raw ) != 0 ) {
		return -1;
	}

	// Store query to call kad_announce_once() later/multiple times
	return announces_add( query, port, lifetime ) ? 0 : -2;
}

// Lookup known nodes that are nearest to the given id
int kad_lookup( const char query[], IP addr_array[], size_t addr_num ) {
	char hostname[QUERY_MAX_SIZE];
	struct search_t *search;

	log_debug( "Lookup identifier: %s", query );

	// Remove .p2p suffix and convert to lowercase
	if( query_sanitize( hostname, sizeof(hostname), query ) != 0 ) {
		log_debug("query_sanitize error");
		return -1;
	}

	// Find existing or create new item
	search = searches_start( hostname );

	if( search == NULL ) {
		// Failed to create a new search
		log_debug("searches_start error");
		return -1;
	}

	// Search was just started (time is only updated in the main loop)
	if( search->start_time == time_now_sec() ) {
		// Search own announced values
		kad_lookup_local_values( search );

		// Start a new DHT search
		dht_lock();
		dht_search( search->id, 0, gconf->af, dht_callback_func, NULL );
		dht_unlock();
	}

	// Collect addresses to be returned
	return searches_collect_addrs( search, addr_array, addr_num );
}

#if 0
/*
* Lookup the address of the node whose node id matches id.
* The lookup will be performed on the results of kad_lookup().
* The port in the returned address refers to the kad instance.
*/
int kad_lookup_node( const char query[], IP *addr_return ) {
	uint8_t id[SHA1_BIN_LENGTH];
	struct search *sr;
	int i, rc;

	if( strlen( query ) != SHA1_HEX_LENGTH || !str_isHex( query, SHA1_HEX_LENGTH ) ) {
		return -1;
	}

	bytes_from_hex( id, query, SHA1_HEX_LENGTH );

	dht_lock();

	rc = 1;
	sr = searches;
	while( sr ) {
		if( sr->af == gconf->af && id_equal( sr->id, id ) ) {
			for( i = 0; i < sr->numnodes; ++i ) {
				if( id_equal( sr->nodes[i].id, id ) ) {
					memcpy( addr_return, &sr->nodes[i].ss, sizeof(IP) );
					rc = 0;
					goto done;
				}
			}
			break;
		}
		sr = sr->next;
	}

	done:;

	dht_unlock();

	return rc;
}
#endif

int kad_blacklist( const IP* addr ) {

	dht_lock();
	blacklist_node( NULL, (struct sockaddr *) addr, sizeof(IP) );
	dht_unlock();

	return 0;
}

// Export known nodes; the maximum is 200 nodes
int kad_export_nodes( IP addr_array[], size_t num ) {
	IP4 *addr4;
	IP6 *addr6;
	int num4;
	int num6;
	int n;

	if( gconf->af == AF_INET6 ) {
		num6 = MIN(num, 200);
		addr6 = calloc( num6, sizeof(IP6) );
		num4 = 0;
		addr4 = NULL;
	} else {
		num6 = 0;
		addr6 = NULL;
		num4 = MIN(num, 200);
		addr4 = calloc( num4, sizeof(IP4) );
	}

	dht_lock();
	dht_get_nodes( addr4, &num4, addr6, &num6 );
	dht_unlock();

	if( gconf->af == AF_INET6 ) {
		for( n = 0; n < num6; ++n ) {
			memcpy( &addr_array[n], &addr6[n], sizeof(IP6) );
		}
		free( addr6 );
	} else {
		for( n = 0; n < num4; ++n ) {
			memcpy( &addr_array[n], &addr4[n], sizeof(IP4) );
		}
		free( addr4 );
	}

	return n;
}

// Print buckets (leaf/finger table)
void kad_debug_buckets( int fd ) {
	struct bucket *b;
	struct node *n;
	int i, j;

	dht_lock();

	b = (gconf->af == AF_INET) ? buckets : buckets6;
	for( j = 0; b; ++j ) {
		dprintf( fd, " Bucket: %s\n", str_id( b->first ) );

		n = b->nodes;
		for( i = 0; n; ++i ) {
			dprintf( fd, "   Node: %s\n", str_id( n->id ) );
			dprintf( fd, "    addr: %s\n", str_addr( &n->ss ) );
			dprintf( fd, "    pinged: %d\n", n->pinged );
			n = n->next;
		}
		dprintf( fd, "  Found %d nodes.\n", i );
		b = b->next;
	}
	dprintf( fd, " Found %d buckets.\n", j );

	dht_unlock();
}

// Print searches
void kad_debug_searches( int fd ) {
	struct search *s;
	int i;
	int j;

	dht_lock();

	s = searches;
	for( j = 0; s; ++j ) {
		dprintf( fd, " Search: %s\n", str_id( s->id ) );
		dprintf( fd, "  af: %s\n", (s->af == AF_INET) ? "AF_INET" : "AF_INET6" );
		dprintf( fd, "  port: %hu\n", s->port );
		//dprintf( fd, "  done: %d\n", s->done );
		for( i = 0; i < s->numnodes; ++i ) {
			struct search_node *sn = &s->nodes[i];
			dprintf( fd, "   Node: %s\n", str_id(sn->id ) );
			dprintf( fd, "    addr: %s\n", str_addr( &sn->ss ) );
			dprintf( fd, "    pinged: %d\n", sn->pinged );
			dprintf( fd, "    replied: %d\n", sn->replied );
			dprintf( fd, "    acked: %d\n", sn->acked );
		}
		dprintf( fd, "  Found %d nodes.\n", i );
		s = s->next;
	}
	dprintf( fd, " Found %d searches.\n", j );

	dht_unlock();
}

// Print announced ids we have received
void kad_debug_storage( int fd ) {
	struct storage *s;
	struct peer* p;
	IP addr;
	int i, j;

	dht_lock();

	s = storage;
	for( j = 0; s; ++j ) {
		dprintf( fd, " ID: %s\n", str_id(s->id ));
		for( i = 0; i < s->numpeers; ++i ) {
			p = &s->peers[i];
			to_addr( &addr, &p->ip, p->len, htons( p->port ) );
			dprintf( fd, "   Peer: %s\n", str_addr( &addr )  );
		}
		dprintf( fd, "  Found %d peers.\n", i );
		s = s->next;
	}
	dprintf( fd, " Found %d stored hashes from received announcements.\n", j );

	dht_unlock();
}

void kad_debug_blacklist( int fd ) {
	int i;

	dht_lock();

	for( i = 0; i < (next_blacklisted % DHT_MAX_BLACKLISTED); i++ ) {
		dprintf( fd, " %s\n", str_addr( &blacklist[i] ) );
	}

	dprintf( fd, " Found %d blacklisted addresses.\n", i );

	dht_unlock();
}

void kad_debug_constants( int fd ) {
	dprintf( fd, "DHT_SEARCH_EXPIRE_TIME: %d\n", DHT_SEARCH_EXPIRE_TIME );
	dprintf( fd, "DHT_MAX_SEARCHES: %d\n", DHT_MAX_SEARCHES );

	// Maximum number of announced hashes we track
	dprintf( fd, "DHT_MAX_HASHES: %d\n", DHT_MAX_HASHES );

	// Maximum number of peers for each announced hash we track
	dprintf( fd, "DHT_MAX_PEERS: %d\n", DHT_MAX_PEERS );

	// Maximum number of blacklisted nodes
	dprintf( fd, "DHT_MAX_BLACKLISTED: %d\n", DHT_MAX_BLACKLISTED );
}

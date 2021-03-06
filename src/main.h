
#ifndef _MAIN_H_
#define _MAIN_H_


#include <netinet/in.h>

#define MAIN_SRVNAME "kadnode"
#define MAIN_VERSION "2.0.2"

#define SHA1_BIN_LENGTH 20
#define SHA1_HEX_LENGTH (2 * SHA1_BIN_LENGTH)

// Default addresses and ports
#define LPD_ADDR4 "239.192.152.143"
#define LPD_ADDR6 "ff15::efc0:988f"
#define LPD_PORT 6771
#define DHT_PORT 6881
#define CMD_PORT 1700
#define DNS_PORT 3535
#define NSS_PORT 4053

#define QUERY_TLD_DEFAULT ".p2p"
#define QUERY_MAX_SIZE 256

typedef struct sockaddr_storage IP;
typedef struct sockaddr_in IP4;
typedef struct sockaddr_in6 IP6;


void main_setup( void );
void main_free( void );


#endif // _MAIN_H_

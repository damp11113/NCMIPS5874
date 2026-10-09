/*
 * lwIP options for the NCAPPS network service (net/net.c): one WiFi
 * Ethernet-style interface, IPv4, DHCP, DNS, TCP / UDP / raw sockets,
 * FreeRTOS (tcpip thread). Memory comes from static arrays in the
 * launcher's own area (never the apps' heap).
 */
#ifndef NET_LWIPOPTS_H
#define NET_LWIPOPTS_H

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_TCPIP_CORE_LOCKING_INPUT   0

/* memory */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (192 * 1024)
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                128
#define MEMP_NUM_NETBUF                 16
#define MEMP_NUM_NETCONN                24
#define MEMP_NUM_TCPIP_MSG_API          16
#define MEMP_NUM_TCPIP_MSG_INPKT        64
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define PBUF_POOL_SIZE                  64

/* protocols */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define ETHARP_SUPPORT_STATIC_ENTRIES   0
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_ACD                        0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        1
#define DNS_TABLE_SIZE                  8
#define DNS_MAX_SERVERS                 2
#define LWIP_BROADCAST_PING             0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1

#define TCP_MSS                         1460
#define TCP_WND                         (16 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_WND_SCALE                  0

/* interface */
#define LWIP_NETIF_HOSTNAME             1           /* DHCP option 12 "ncwifi" */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_TX_SINGLE_PBUF       1           /* linkoutput gets one buffer */
#define LWIP_SINGLE_NETIF               1
#define ETH_PAD_SIZE                    0

/* APIs */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             0           /* lwip_* names only */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_SOCKET_SELECT              1
#define LWIP_SOCKET_POLL                1
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  0
#define LWIP_TCP_KEEPALIVE              1
#define SO_REUSE                        1
#define LWIP_NETIF_API                  0
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

/* threads (sys_arch.c, FreeRTOS): stack sizes in bytes */
#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          (32 * 1024)
#define TCPIP_THREAD_PRIO               3
#define TCPIP_MBOX_SIZE                 64
#define DEFAULT_THREAD_STACKSIZE        (16 * 1024)
#define DEFAULT_RAW_RECVMBOX_SIZE       16
#define DEFAULT_UDP_RECVMBOX_SIZE       16
#define DEFAULT_TCP_RECVMBOX_SIZE       32
#define DEFAULT_ACCEPTMBOX_SIZE         8

#define LWIP_CHKSUM_ALGORITHM           3

#endif

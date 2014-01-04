#include "stdio.h" 
#include "stdlib.h" 
#include "string.h" 

#include <sys/socket.h>
#include <netinet/in.h> 
#include <netdb.h> 

#include "assert.h"

#include "sx_mgmt_rtsp.h"

#define RTSP_BUF_SIZE_MAX   2048
#define MGMT_RTSP_PORT      8554

#define OPTIONS             "OPTIONS"
#define DESCRIBE            "DESCRIBE"
#define SETUP               "SETUP"
#define PLAY                "PLAY"
#define TEARDOWN            "TEARDOWN"


typedef enum
{
    RTSP_MSG_OPTIONS, 
    RTSP_MSG_DESCRIBE, 
    RTSP_MSG_SETUP,
    RTSP_MSG_PLAY, 
    RTSP_MSG_TEARDOWN, 

} eRTSP_MSG; 


typedef struct
{
    unsigned char   in_use; 
    int             tcp_sock; 
    unsigned int    client_ip; 
    unsigned short  client_port; 
    unsigned char   client_ip_str[16]; 
    pthread_t       rtsp_thread; 

} sMGMT_RTSP_SESSION; 


typedef struct
{
    pthread_t           tcp_thread; 
    int                 rtsp_sock; 
    fSX_MGMT_RTSP_CBACK    user_cback; 
    void               *user_arg; 
    sMGMT_RTSP_SESSION  session[32]; 

} sMGMT_RTSP_CBLK; 


// Control block. 
static sMGMT_RTSP_CBLK f_cblk; 


static unsigned int session_instance_alloc(
    void
    )
{
    unsigned int i;


    // TODO: mutex

    for(i = 0; i < 32; i++)
    {
        if(!f_cblk.session[i].in_use)
        {
            f_cblk.session[i].in_use = 1;

            return i;
        }
    }

    assert(0);
}


static void session_instance_free(
    unsigned int    index
    )
{
    // TODO: mutex

    f_cblk.session[index].in_use = 0;
}


static int get_cseq(
    char   *msg
    )
{
#define CSEQ    "CSeq: "

    char *temp = strstr(msg, CSEQ); 
    temp += strlen(CSEQ); 

    return *temp - '0'; 
}


static char * get_session(
    char   *msg
    )
{
#define SESSION "Session: "

    char *temp = strstr(msg, SESSION); 

    char * str = malloc(9); 

    temp += strlen(SESSION); 

    memcpy(str, temp, 8); 
    str[8] = 0; 

    return str; 
}


static char * get_url(
    char   *msg
    )
{
#define URL     "rtsp:"

    char *start = strstr(msg, URL);

    char *end = strstr(start, " "); 

    char * str = malloc(end - start + 1); 

    memcpy(str, start, end-start); 
    str[end - start] = 0; 
    
    return str; 
}


static unsigned short get_client_port(
    char   *msg
    )
{
#define CLIENT_PORT "client_port="

    char *temp = strstr(msg, CLIENT_PORT); 
    temp += strlen(CLIENT_PORT);

    int port = 0; 
    do
    {
        // 64346 
        port *= 10; 
        port += *temp - '0'; 

        temp++; 

    } while (*temp != '-'); 

    return port; 
}


static char * options_handler(
    char   *msg_rx
    )
{
    unsigned int cseq = get_cseq(msg_rx); 

    char * out = malloc(RTSP_BUF_SIZE_MAX); 

    snprintf(out, 
             RTSP_BUF_SIZE_MAX, 
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
             cseq); 
    // TODO: GET_PARAMETER and SET_PARAMETER

    return out; 
}


static char * describe_handler(
    sMGMT_RTSP_SESSION *session, 
    char               *msg_rx
    )
{
    char   *sdp_str = "v=0\r\n"
                  "o=- 1364163928239452 1 IN IP4 %s\r\n"
                  "s=H.264 Video, streamed by the LIVE555 Media Server\r\n"
                  "i=pi_encode.264\r\n"
                  "t=0 0\r\n"
                  "a=tool:LIVE555 Streaming Media v2013.01.25\r\n"
                  "a=type:broadcast\r\n"
                  "a=control:*\r\n"
                  "a=range:npt=0-\r\n"
                  "a=x-qt-text-nam:H.264 Video, streamed by the LIVE555 Media Server\r\n"
                  "a=x-qt-text-inf:pi_encode.264\r\n"
                  "m=video 0 RTP/AVP 96\r\n"
                  "c=IN IP4 0.0.0.0\r\n"
                  "b=AS:500\r\n"
                  "a=rtpmap:96 H264/90000\r\n"
                  "a=fmtp:96 packetization-mode=1;profile-level-id=64001E;sprop-parameter-sets=J2QAHqwrQFAX/LAPEiag,KO4CXLA=\r\n"
                  "a=control:track1\r\n";

    char    sdp[2048]; 

    sprintf(sdp, 
            sdp_str, 
            session->client_ip_str); 

    unsigned int cseq = get_cseq(msg_rx); 

    unsigned char * url = get_url(msg_rx); 

    char * out = malloc(RTSP_BUF_SIZE_MAX); 

    snprintf(out, 
             RTSP_BUF_SIZE_MAX, 
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Content-Base: %s\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %d\r\n\r\n"
             "%s",
             cseq, 
             url,
             strlen(sdp),
             sdp);

    free(url); 

    return out; 
}


static char * setup_handler(
    sMGMT_RTSP_SESSION *session, 
    char               *msg_rx, 
    unsigned short     *client_port
    )
{
    unsigned int cseq = get_cseq(msg_rx); 

    unsigned short dst_port = get_client_port(msg_rx); 

    // Allocate buffer. 
    char * out = malloc(RTSP_BUF_SIZE_MAX); 

    // Format message. 
    snprintf(out, 
            RTSP_BUF_SIZE_MAX, 
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Transport: RTP/AVP;unicast;destination=%s;source=%s;client_port=%d-%d;server_port=%d-%d\r\n"
            "Session: %08X\r\n\r\n",
            cseq, 
            "192.168.1.76", 
            session->client_ip_str, 
            dst_port,
            dst_port+1, 
            62000, 
            62000+1, 
            0x11223344); 

    *client_port = dst_port; 

    return out; 
}


static char * play_handler(
    char   *msg_rx
    )
{
    unsigned int cseq = get_cseq(msg_rx); 

    char * session = get_session(msg_rx); 

    char * out = malloc(RTSP_BUF_SIZE_MAX); 

     // Fill in the response:
    snprintf(out, RTSP_BUF_SIZE_MAX,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "%s"
            "Session: %08X\r\n"
            "%s\r\n",
            cseq, 
            "Range: npt=0.000-\r\n", 
            session, 
            "RTP-Info: url=rtsp://192.168.1.72:8554/pi_encode.264/track1;seq=61719;rtptime=4288250384\r\n"); 

    free(session); 

    return out; 
}


static char * teardown_handler(
    char   *msg_rx
    )
{
    unsigned int cseq = get_cseq(msg_rx); 

    char * out = malloc(RTSP_BUF_SIZE_MAX); 

     // Fill in the response:
    snprintf(out, RTSP_BUF_SIZE_MAX,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "\r\n",
            cseq); 

    return out; 
}


static eRTSP_MSG get_msg_type(
    char *msg_rx
    )
{
    if(strncmp(msg_rx, OPTIONS, strlen(OPTIONS)) == 0)
    {
        return RTSP_MSG_OPTIONS; 
    }
    else if(strncmp(msg_rx, DESCRIBE, strlen(DESCRIBE)) == 0)
    {
        return RTSP_MSG_DESCRIBE; 
    }
    else if(strncmp(msg_rx, SETUP, strlen(SETUP)) == 0)
    {
        return RTSP_MSG_SETUP; 
    }
    else if(strncmp(msg_rx, PLAY, strlen(PLAY)) == 0)
    {
        return RTSP_MSG_PLAY; 
    }
    else if(strncmp(msg_rx, TEARDOWN, strlen(TEARDOWN)) == 0)
    {
        return RTSP_MSG_TEARDOWN; 
    }
    else
    {
        assert(0); 
    }
}


// RTSP Server thread. 
static void rtsp_server_thread(
    void * arg
    )
{
    unsigned int            id; 
    unsigned int            addr_len;
    struct sockaddr_in      client_addr;
    char                    msg_rx[RTSP_BUF_SIZE_MAX]; 
    char                   *msg_tx; 
    unsigned short          client_port; 
    uSX_MGMT_RTSP_EVENT_DATA   event_data; 
    int                     rv; 


    id = (unsigned int) arg; 

    sprintf(f_cblk.session[id].client_ip_str, 
            "%d.%d.%d.%d",
            ((unsigned char *) &f_cblk.session[id].client_ip)[0], 
            ((unsigned char *) &f_cblk.session[id].client_ip)[1], 
            ((unsigned char *) &f_cblk.session[id].client_ip)[2], 
            ((unsigned char *) &f_cblk.session[id].client_ip)[3]); 

    logger_log("MGMT_RTSP: New RTSP server thread. [id = %d]", id); 

    logger_log("MGMT_RTSP: Client IP: %s", f_cblk.session[id].client_ip_str); 

    while(1)
    {
        // Get received message. 
        rv = read(f_cblk.session[id].tcp_sock, msg_rx, RTSP_BUF_SIZE_MAX); 
        if(rv < 1)
        {
            logger_log("MGMT_RTSP: Client terminated TCP connection. [client IP: %s]", 
                    f_cblk.session[id].client_ip_str); 
            break; 
        }

        // Log request. 
        logger_log("MGMT_RTSP: RTSP Request [session ID = %d]:", id); 
        logger_log("%s", msg_rx); 

        // Get message type. 
        eRTSP_MSG msg_type = get_msg_type(msg_rx); 
        switch(msg_type)
        {
            case RTSP_MSG_OPTIONS: 
            {
                msg_tx = options_handler(msg_rx); 
                break; 
            }
            case RTSP_MSG_DESCRIBE:
            {
                msg_tx = describe_handler(&f_cblk.session[id], 
                                          msg_rx); 
                break; 
            }
            case RTSP_MSG_SETUP:
            {
                msg_tx = setup_handler(&f_cblk.session[id], 
                        msg_rx, 
                        &client_port); 
                f_cblk.session[id].client_port = client_port; 
                break; 
            }
            case RTSP_MSG_PLAY:
            {
                msg_tx = play_handler(msg_rx); 
                break; 
            }
            case RTSP_MSG_TEARDOWN:
            {
                msg_tx = teardown_handler(msg_rx); 
                break; 
            }
            default:
            {
                // TODO: asserting on network data is never good. 
                assert(0); 
            }
        }

        logger_log("MGMT_RTSP: RTSP Response [session ID = %d]:", id); 
        logger_log("%s", msg_tx); 

        // Send RTSP response. 
        write(f_cblk.session[id].tcp_sock, msg_tx, strlen(msg_tx)); 

        // Free sent message. 
        free(msg_tx); 

        // Perform appropriate callback. 
        if(msg_type == RTSP_MSG_PLAY)
        {
            // Setup return data. 
            event_data.play.id      = id; 
            event_data.play.ip      = f_cblk.session[id].client_ip; 
            event_data.play.port    = f_cblk.session[id].client_port; 

            logger_log("####### "); 

            // Callback with event data. 
            f_cblk.user_cback(f_cblk.user_arg, 
                    MGMT_RTSP_EVENT_PLAY, 
                    &event_data); 
        }

        if(msg_type == RTSP_MSG_TEARDOWN)
        {
            event_data.teardown.id = id; 

            // Callback with event data. 
            f_cblk.user_cback(f_cblk.user_arg, 
                    MGMT_RTSP_EVENT_TEARDOWN, 
                    &event_data); 

            // Free resource.
            session_instance_free(id);

            break; 
        }
    }
}





static void rtsp_thread_create(
    void   *arg
    )
{
    pthread_create(&f_cblk.session[(unsigned int) arg].rtsp_thread, NULL, (void *) &rtsp_server_thread, arg); 
}


static void tcp_listener_thread(
    void * arg
    )
{
    unsigned int            addr_len;
    struct sockaddr_in      client_addr;
    char                    msg_rx[RTSP_BUF_SIZE_MAX]; 
    char                   *msg_tx; 
    unsigned short          client_port; 
    uSX_MGMT_RTSP_EVENT_DATA   event_data; 
    int                     rv; 


    // Create socket. 
    f_cblk.rtsp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(f_cblk.rtsp_sock != -1);

    // Setup address. 
    client_addr.sin_family      = AF_INET;
    client_addr.sin_port        = htons(MGMT_RTSP_PORT);
    client_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(client_addr.sin_zero), 8);

    // Bind to address. 
    int rc = bind(f_cblk.rtsp_sock, 
            (struct sockaddr *) &client_addr,
            sizeof(client_addr)); 
    assert(rc == 0);

    // Listening for traffic. 
    listen(f_cblk.rtsp_sock, 5); 

    while(1)
    {
        // Accept new TCP connection from specified address. 
        int client_addr_len = sizeof(client_addr); 
        int tcp_sock = accept(f_cblk.rtsp_sock, 
                              (struct sockaddr *) &client_addr, 
                              &client_addr_len); 

        logger_log("MGMT_RTSP: Received new TCP connection, initiating new RTSP sever instance..."); 

        // Allocate session instance. 
        int session = session_instance_alloc(); 

        f_cblk.session[session].client_ip   = client_addr.sin_addr.s_addr; 
        f_cblk.session[session].tcp_sock    = tcp_sock; 

        // Create new TCP socket. 
        rtsp_thread_create((void *) session); 
    }
}


static void tcp_listener_create(
    )
{
    pthread_create(&f_cblk.tcp_thread, NULL, (void *) &tcp_listener_thread, NULL); 
}


void sx_mgmt_rtsp_init(
    fSX_MGMT_RTSP_CBACK    user_cback, 
    void               *user_arg
    )
{
    printf("mgmt_rtsp_init(): Inovked.\n"); 

    // Cache user callback. 
    f_cblk.user_cback = user_cback; 
    f_cblk.user_arg  = user_arg; 
}


void sx_mgmt_rtsp_open(
    )
{
    printf("mgmt_rtsp_open(): Inovked.\n"); 

    // Create RTSP thread. 
    tcp_listener_create(); 
}


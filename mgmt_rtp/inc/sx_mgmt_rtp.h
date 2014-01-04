#ifndef _MGMT_RTP_H_
#define _MGMT_RTP_H_



extern void sx_mgmt_rtp_init(
    void
    );

extern void sx_mgmt_rtp_open(
    void
    );

extern void sx_mgmt_rtp_activate(
    unsigned int    id, 
    unsigned int    ip,
    unsigned short  port
    );

extern void sx_mgmt_rtp_reset(
    unsigned int    id
    );

#endif // _MGMT_RTP_H_

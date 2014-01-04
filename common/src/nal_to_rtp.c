#include "stdio.h" 
#include "stdlib.h" 
#include "string.h" 
#include "nal_to_rtp.h"

#define FUA_FRAGMENT_START  0x80
#define FUA_FRAGMENT_MIDDLE 0x00
#define FUA_FRAGMENT_END    0x40


// Module control block. 
typedef struct
{
    unsigned int    timestamp; 
    unsigned int    sequence_number; 

} sH264_TO_RTP_CBLK; 


// Function definition to allocate a node. 
static sRTP_PKT_NODE  *node_malloc(
    void
    )
{
    sRTP_PKT_NODE  *node; 


    node = malloc(sizeof(sRTP_PKT_NODE)); 

    node->next          = NULL; 
    node->rtp_pkt_len   = 0; 

    return node;
}


// Set RTP header. 
static void rtp_header_set(
    sH264_TO_RTP_CBLK  *cblk,
    sRTP_HEADER        *hdr,
    unsigned char       m_bit_set
    )
{
    hdr->version_p_x_cc     = 0x80; 
    hdr->m_pt               = m_bit_set << 7 | 0x60; 
    hdr->sequence_number    = ntohs(cblk->sequence_number++);
    hdr->timestamp          = ntohl(cblk->timestamp);
    hdr->ssrc               = 0x1; 
    hdr->csrc               = 0x2; 

    if(m_bit_set)
    {
        cblk->timestamp += 3600;
    }
}


// Set the FU-A unit header. 
static void h264_fua_header_set(
    unsigned char  *fua_header, 
    unsigned char   nri, 
    unsigned char   pos, 
    unsigned char   nal_type
    )
{
    // Construct fragment unit indicator. 

    // Set NRI. 
    fua_header[0] = nri << 5; 

    // Set NAL type FU-A. 
    fua_header[0] |= 28; 

    // Construct Fragment unit header. 

    fua_header[1] = pos; 
    fua_header[1] |= nal_type; 
}


// Get single packet slice. 
static sRTP_PKT_NODE * get_single_pkt_chain(
    sH264_TO_RTP_CBLK  *cblk,
    unsigned char      *h264_frame,
    unsigned int        h264_frame_len
    )
{
    sRTP_PKT_NODE   *node; 


    // Get node. 
    node = node_malloc(); 

    // Set header.
    rtp_header_set(cblk, &node->rtp_pkt.header, 1);

    // Set payload. 
    memcpy(&node->rtp_pkt.payload.bytes[0], h264_frame, h264_frame_len); 

    // Set pkt length. 
    node->rtp_pkt_len = sizeof(sRTP_HEADER) + h264_frame_len; 

    return node; 
}


// Get multi packet slice. 
static sRTP_PKT_NODE * get_multi_pkt_chain(
    sH264_TO_RTP_CBLK  *cblk,
    unsigned char      *h264_frame,
    unsigned int        h264_frame_len
    )
{
    unsigned int    bytes_remaining; 
    unsigned char   nri; 
    unsigned char   nal_type; 
    unsigned char   marker_bit_set; 
    unsigned int    curr_index; 
    unsigned int    pkt_size_left; 
    unsigned char   pos; 
    unsigned int    bytes_to_copy; 
    sRTP_PKT_NODE  *node; 
    sRTP_PKT_NODE  *head; 


    // Extract NRI. 
    nri         = (h264_frame[0] & 0x60) >> 5; 

    // Extract NAL type. 
    nal_type    = (h264_frame[0] & 0x1f); 

    // Header is parsed. 
    bytes_remaining = h264_frame_len - 1; 

    // Header is parsed. 
    curr_index = 1; 

    do
    {
        // Bytes left in pkt. 
        pkt_size_left = RTP_PAYLOAD_SIZE; 

        // Determine the fragment position 
        marker_bit_set = 0; 
        if(bytes_remaining == (h264_frame_len - 1))
        {
            // First iteration. 
            pos = FUA_FRAGMENT_START; 
        }
        else if (bytes_remaining < RTP_PAYLOAD_SIZE)
        {
            // Last iteratoin. 
            marker_bit_set = 1; 
            pos = FUA_FRAGMENT_END; 
        }
        else
        {
            pos = FUA_FRAGMENT_MIDDLE; 
        }

        // Locate node. 
        if(pos == FUA_FRAGMENT_START)
        {
            // First node. 
            node = node_malloc(); 

            // Cache head. 
            head = node; 
        }
        else
        {
            // Allocate next node. 
            node->next = node_malloc(); 

            // Advance. 
            node = node->next; 
        }

        // Set RTP header. 
        rtp_header_set(cblk, &node->rtp_pkt.header, marker_bit_set);

        // Set FUA unit header.
        h264_fua_header_set(&node->rtp_pkt.payload.bytes[0], 
                            nri, 
                            pos, 
                            nal_type); 

        // FUA header is 2 bytes. 
        pkt_size_left -= 2; 

        // Copy as many bytes as we can. 
        bytes_to_copy = (pkt_size_left < bytes_remaining) ? 
            pkt_size_left : bytes_remaining; 

        // Copy payload. 
        memcpy(&node->rtp_pkt.payload.bytes[2], 
                &h264_frame[curr_index], 
                bytes_to_copy); 

        node->rtp_pkt_len = sizeof(sRTP_HEADER) + 2 + bytes_to_copy; 

        // Advance h264 frame index. 
        curr_index += bytes_to_copy; 

        // Decrement bytes to copy. 
        bytes_remaining -= bytes_to_copy; 

    } while (!marker_bit_set); 

    return head; 
}


void * sx_nal_to_rtp_util_create(
    void
    )
{
    void  *cblk = malloc(sizeof(sH264_TO_RTP_CBLK));

    memset(cblk, 0, sizeof(sH264_TO_RTP_CBLK));

    return cblk;
}

void sx_nal_to_rtp_util_destroy(
    void   *ptr
    )
{
    free(ptr);
}


// Get chain. 
sRTP_PKT_NODE * sx_nal_to_rtp_util_get(
    void           *arg,
    unsigned char  *h264_frame, 
    unsigned int    h264_frame_len
    )
{
    sH264_TO_RTP_CBLK  *cblk = arg;


    if(h264_frame_len <= RTP_PAYLOAD_SIZE)
    {
        // Single packet. 
        return get_single_pkt_chain(cblk, h264_frame, h264_frame_len);
    }

    // Multi packet chain. 
    return get_multi_pkt_chain(cblk, h264_frame, h264_frame_len);
}
 

 void sx_nal_to_rtp_util_free(
     sRTP_PKT_NODE * head 
    )
 {
     sRTP_PKT_NODE * node; 
     sRTP_PKT_NODE * temp; 


     node = head; 
     do
     {
         temp = node; 
         node = node->next; 

         free(temp); 

     } while(node != NULL); 
 }

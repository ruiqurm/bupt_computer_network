#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2500
#define ACK_TIMER 1200
#define start_ack_timer() start_ack_timer(ACK_TIMER)
// #define flush_ack_timer() stop_ack_timer();start_ack_timer();

//如果无piggybacking，单独发ACK
#define DEBU
//#define DEBUG_PRINT
#ifdef DEBUG
    #ifdef CHAN_DELAY
    #undef CHAN_DELAY
    #define CHAN_DELAY 10
    #else
    #define CHAN_DELAY 10  /
    #endif

    #ifdef CHAN_BPS
    #undef CHAN_BPS
    #define CHAN_BPS 10
    #else
    #define CHAN_BPS 10  /
    #endif

#endif
// #define CHAN_BPS   8000  

#define MAX_SEQ 31
// const int MAX_WD_SIZE = (MAX_SEQ + 1) >> 1;
#define MAX_WD_SIZE 16
#define inc(x)  x = (x+1) % (MAX_SEQ+1)
#define pred(x) ((x + MAX_SEQ) % (MAX_SEQ + 1))
#define succ(x) ((x+1) % (MAX_SEQ+1))
#define ack_now ((frame_expected + MAX_SEQ) % (MAX_SEQ + 1))
#define between(a,b,c) ((a<=b && b<c)|| \
                    (c < a&& b>=a)|| \
                    (b<c&&c<a))
struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
};
typedef unsigned char bool;
static unsigned char next_seq = 0,ack_expected=0, buffer[PKT_LEN], nbuffered=0;
static unsigned char frame_expected = 0;//接收窗口始
static unsigned char frame_expected_max = MAX_WD_SIZE-1;//接收窗口结束
static int phl_ready = 0;
static int nak_sended = 0;
struct{
    bool arrived;
    char buffer[PKT_LEN];
}recv_buffer[MAX_WD_SIZE];//接收方窗口缓存
struct{
    char buffer[PKT_LEN];
}send_buffer[PKT_LEN];
static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(int next_seq,int frame_expected,char buffer[])
{
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = next_seq;//局部变量next_seq
    s.ack =  ack_now;//局部变量frame_expected的上一个
    memcpy(s.data, buffer, PKT_LEN);

    dbg_frame("Send DATA seq=%d,ack=%d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(next_seq, DATA_TIMER);
    stop_ack_timer();
}

static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = ack_now;
    s.seq =0;
    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}
static void send_nak_frame(void)
{
    nak_sended = 1;
    struct FRAME s;

    s.kind = FRAME_NAK;
    s.ack = ack_now;
    s.seq = 0;
    dbg_frame("\033[31mSend NAK  %d\033[0m\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}
int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;
    

    protocol_init(argc, argv); 
    lprintf("Designed by Jiang Yanjun, build: " __DATE__"  "__TIME__"\n");
    lprintf("Edited By Qiu Qichen, 2021.4.20\n");
    // disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);
        #ifdef DEBUG_PRINT
        lprintf("发送窗口: 下界：ack_expected:%d 上界：next_seq:%d   接收窗口:下界(frame_expected):%d   上界(frame_expected_max) %d\n",
                    ack_expected,next_seq,frame_expected,frame_expected_max);
        #endif
        switch (event) {
        case NETWORK_LAYER_READY:
            if(nbuffered <= MAX_WD_SIZE){
                get_packet(send_buffer[next_seq % MAX_WD_SIZE].buffer);//从物理层提取数据
                // memcpy(send_buffer[next_seq % MAX_WD_SIZE].buffer,buffer,sizeof(buffer));
                nbuffered++;
                send_data_frame(next_seq,frame_expected,send_buffer[next_seq % MAX_WD_SIZE].buffer);
                inc(next_seq);
            }
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) { 
                dbg_event("\033[31mRecv DATA %d %d, ID %d, Bad CRC Checksum\033[0m,",f.seq, f.ack, *(short *)f.data);
                if (!nak_sended){
                    send_nak_frame();//通知重发
                }
                break;
            }            
        
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq!=frame_expected && !nak_sended){
                    send_nak_frame();
                }   

                if (between(frame_expected,f.seq,frame_expected_max) && !recv_buffer[f.seq % MAX_WD_SIZE].arrived){
                    //在window中，且未受过
                    recv_buffer[f.seq % MAX_WD_SIZE].arrived = 1;
                    memcpy(recv_buffer[f.seq%MAX_WD_SIZE].buffer,f.data,len-7);
                    while(recv_buffer[frame_expected % MAX_WD_SIZE].arrived){//推动接收窗口
                        nak_sended = 0;
                        recv_buffer[frame_expected % MAX_WD_SIZE].arrived = 0;
                        put_packet(recv_buffer[frame_expected % MAX_WD_SIZE].buffer, len - 7);
                        dbg_frame("%d to network layer.\n", frame_expected);
                        inc(frame_expected);
                        inc(frame_expected_max);
                        start_ack_timer();
                        // dbg_frame("Now Recv window:%d - %d\n", frame_expected,frame_expected_max);
                    }
                }else{
                    dbg_event("Departed Data %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                }
                // if (f.seq == frame_expected) {
                //     // put_packet(f.data, len - 7);
                //     // frame_expected = 1 - frame_expected;
                // }
                // send_ack_frame();
            } 
            if (f.kind==FRAME_NAK && between(ack_expected,succ(f.ack),next_seq)){
                dbg_event("\033[31mRecv NAK;try to send %d\033[0m\n",succ(f.ack));
                
                send_data_frame(succ(f.ack),frame_expected,send_buffer[succ(f.ack)%MAX_WD_SIZE].buffer);
            }
            while(between(ack_expected,f.ack,next_seq)){//累计确认
                dbg_frame("Recv ACK  %d\n", ack_expected);
                nbuffered--;
                stop_timer(ack_expected);
                inc(ack_expected);
            }
            break; 

        case DATA_TIMEOUT:
            dbg_event("\033[31m---- DATA %d timeout\033[0m\n", arg);
            dbg_frame("Re-send ack_expected %d",ack_expected);
            send_data_frame(ack_expected,frame_expected,send_buffer[ack_expected % MAX_WD_SIZE].buffer);
            // dbg_frame("---- Retry and send all packets from %d to %d\n", ack_expected,pred(next_seq)); 
            // for(int i =ack_expected;i!=next_seq;inc(i)){
            //    send_data_frame(i,frame_expected,send_buffer[i % MAX_WD_SIZE].buffer);
            // }
            //全部重传
            break;
        case ACK_TIMEOUT:
            dbg_event("\033[31m---- ACK timeout\033[0m\n", arg); 
            send_ack_frame();
            break;
        default:
            //do nothing
            break;
        }
        if (nbuffered < MAX_WD_SIZE && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}

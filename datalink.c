
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"



/**
 * 
 * 全局数据
 * 
 */

#define DATA_TIMER 3500     //超时时间
#define ACK_TIMER  1000     //ACK超时时间

#define MAX_SEQ 31          //最大序号
#define MAX_WD_SIZE 16      //最大窗口

struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
};//帧格式

typedef unsigned char bool;
static unsigned char next_seq = 0,                      //下一个发送帧的序号
                     ack_expected=0,                    //期望收到的ACK序号 
                     nbuffered=0;                       //已发送未确认的数据包数量
// static unsigned buffer[PKT_LEN];
static unsigned char frame_expected = 0;                //接收窗口始
static unsigned char frame_expected_max = MAX_WD_SIZE-1;//接收窗口结束
static int phl_ready = 0;                               //物理层是否完成准备
static int nak_sended = 0;                              //是否发送过NAK

//接收方窗口缓存
struct{
    bool arrived;
    char buffer[PKT_LEN];
}recv_buffer[MAX_WD_SIZE];

//发送方数据缓存
struct{
    char buffer[PKT_LEN];
}send_buffer[MAX_WD_SIZE];


/**
 * 
 * 宏和函数
 * 
 */

//启动ACK计时
#define start_ack_timer() start_ack_timer(ACK_TIMER)

//序号自增1
#define inc(x)  x = (x+1) % (MAX_SEQ+1)
//序号前驱
#define pred(x) ((x + MAX_SEQ) % (MAX_SEQ + 1))
//序号后继
#define succ(x) ((x+1) % (MAX_SEQ+1))

//已确认的最后一个数据序号
#define ack_now ((frame_expected + MAX_SEQ) % (MAX_SEQ + 1))

//是否在中间
#define between(a,b,c) ((a<=b && b<c)|| \
                    (c < a&& b>=a)|| \
                    (b<c&&c<a))

//把帧放到物理层
static void put_frame(unsigned char *frame, int len);
//发送DATA数据帧
static void send_data_frame(int next_seq,int frame_expected,char buffer[]);
//发送ACK帧
static void send_ack_frame(void);
//发送NAK帧
static void send_nak_frame(void);


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
            // 如果网络层有新的数据
            if(nbuffered <= MAX_WD_SIZE){
                get_packet(send_buffer[next_seq % MAX_WD_SIZE].buffer);//从上层提取数据
                nbuffered++;
                send_data_frame(next_seq,frame_expected,send_buffer[next_seq % MAX_WD_SIZE].buffer);
                inc(next_seq);
            }
            break;

        case PHYSICAL_LAYER_READY:
            //物理层状态置位
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            //如果收到帧
            len = recv_frame((unsigned char *)&f, sizeof f);

            //CRC校验
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) { 
                dbg_event("\033[31mRecv DATA %d %d, ID %d, Bad CRC Checksum\033[0m,",f.seq, f.ack, *(short *)f.data);
                //未发过NAK则请求新帧
                if (!nak_sended){
                    send_nak_frame();
                }
                break;
            }            
        
            if (f.kind == FRAME_DATA) {
                //若收到的是数据帧
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);

                //如果是错序的
                //因为没有阻塞问题，先发的帧必然先到
                if (f.seq!=frame_expected && !nak_sended){
                    send_nak_frame();
                }   

                if (between(frame_expected,f.seq,frame_expected_max) && !recv_buffer[f.seq % MAX_WD_SIZE].arrived){
                    //收到的帧在接收窗口中，并且之前未收到过

                    //保存数据
                    recv_buffer[f.seq % MAX_WD_SIZE].arrived = 1;               
                    memcpy(recv_buffer[f.seq%MAX_WD_SIZE].buffer,f.data,len-7);

                    while(recv_buffer[frame_expected % MAX_WD_SIZE].arrived){//推动接收窗口
                        nak_sended = 0; //可以再发新的NAK了，因为未收到的最新帧已经收到

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
            } 
            if (f.kind==FRAME_NAK && between(ack_expected,succ(f.ack),next_seq)){
                //如果这是一个NAK，那么发送当前未确认的最新帧
                dbg_event("\033[31mRecv NAK;try to send %d\033[0m\n",succ(f.ack));
                
                send_data_frame(succ(f.ack),frame_expected,send_buffer[succ(f.ack)%MAX_WD_SIZE].buffer);
            }
            while(between(ack_expected,f.ack,next_seq)){
                // 累积确认
                dbg_frame("Recv ACK  %d\n", ack_expected);
                nbuffered--;
                stop_timer(ack_expected);
                inc(ack_expected);
            }
            break; 

        case DATA_TIMEOUT:
            //超时重传未确认的最新帧
            dbg_event("\033[31m---- DATA %d timeout\033[0m\n", arg);
            dbg_frame("Re-send ack_expected %d",ack_expected);
            send_data_frame(ack_expected,frame_expected,send_buffer[ack_expected % MAX_WD_SIZE].buffer);
            // dbg_frame("---- Retry and send all packets from %d to %d\n", ack_expected,pred(next_seq)); 
            // for(int i =ack_expected;i!=next_seq;inc(i)){
            //    send_data_frame(i,frame_expected,send_buffer[i % MAX_WD_SIZE].buffer);
            // }
            //全部重传（效率比较低）
            break;
        case ACK_TIMEOUT:
            //超时重传ACK帧
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



static void put_frame(unsigned char *frame, int len){
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(int next_seq,int frame_expected,char buffer[]){
    //发送一条数据帧
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = next_seq;//局部变量next_seq
    s.ack =  ack_now;//局部变量frame_expected的上一个
    memcpy(s.data, buffer, PKT_LEN);

    dbg_frame("Send DATA seq=%d,ack=%d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(next_seq, DATA_TIMER);
    stop_ack_timer();//已经捎带确认
}

static void send_ack_frame(void){
    //发送一条ACK
    struct FRAME s;

    s.kind = FRAME_ACK;
    s.ack = ack_now;
    s.seq =0;
    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}
static void send_nak_frame(void){
    nak_sended = 1;
    struct FRAME s;

    s.kind = FRAME_NAK;
    s.ack = ack_now;
    s.seq = 0;
    dbg_frame("\033[31mSend NAK  %d\033[0m\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}
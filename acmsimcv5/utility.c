#include "ACMSim.h"
extern REAL sig_a2;
extern REAL sig_a3;


/* 功能函数 */
// 符号函数
double sign(double x){
    return (x > 0) - (x < 0);    
}
int64 sign_integer(int64 x){
    return (x > 0) - (x < 0);
}
// 浮点数的绝对值函数
double fabs(double x){
    return (x >= 0) ? x : -x;
}
// 判断是否为有效浮点数
int isNumber(double x){
    // This looks like it should always be TRUE, 
    // but it's FALSE if x is an NaN (1.#QNAN0).
    return (x == x); 
    // see https://www.johndcook.com/blog/IEEE_exceptions_in_cpp/ cb: https://stackoverflow.com/questions/347920/what-do-1-inf00-1-ind00-and-1-ind-mean
}


//低通滤波器：测量值，上一步的滤波器输出，时间常数的倒数
REAL _lpf(REAL x, REAL y, REAL tau_inv){
    return y + CL_TS * tau_inv * (x - y);
}
//高通滤波器：测量值，上一步的低通滤波器输出的地址，时间常数的倒数
REAL _hpf(REAL x, REAL *lpf_y, REAL tau_inv){
    *lpf_y = _lpf(x, *lpf_y, tau_inv);
    return x - *lpf_y;
}

// #define MA_SEQUENCE_LENGTH         20 // 20 * CL_TS = window of moving average in seconds
// #define MA_SEQUENCE_LENGTH_INVERSE 0.05
// REAL ENC.MA_qepPosCnt[MA_SEQUENCE_LENGTH];
// REAL sum_qepPosCnt = 0.0;
// unsigned int ENC.cursor=0; // ENC.cursor is between 0~79, and it decides which element in MA queue should be kicked out.
REAL PostionSpeedMeasurement_MovingAvergage(Uint32 QPOSCNT, st_enc *p_enc){
    // EQep1Regs
    #define enc (*p_enc)

    /* Part One: Read QEP Registers to get Position */

        /* 获取绝对位置 [cnt] 用于转子位置解算 */
        enc.encoder_abs_cnt_previous = enc.encoder_abs_cnt;
        enc.encoder_abs_cnt = (int32)QPOSCNT + enc.OffsetCountBetweenIndexAndUPhaseAxis;
        if(enc.encoder_abs_cnt > SYSTEM_QEP_QPOSMAX_PLUS_1) {enc.encoder_abs_cnt -= SYSTEM_QEP_QPOSMAX_PLUS_1;}
        if(enc.encoder_abs_cnt < 0)                         {enc.encoder_abs_cnt += SYSTEM_QEP_QPOSMAX_PLUS_1;}
        enc.theta_d__state = enc.encoder_abs_cnt * CNT_2_ELEC_RAD;
        while(enc.theta_d__state> M_PI) enc.theta_d__state -= 2*M_PI;
        while(enc.theta_d__state<-M_PI) enc.theta_d__state += 2*M_PI;

    /* Part Two: Moving Average with a update period of CL_TS */

        /* 获取位置增量 [cnt] 用于滑动平均转速解算 */
        enc.encoder_incremental_cnt = enc.encoder_abs_cnt - enc.encoder_abs_cnt_previous;
        // 增量超过ppr的一半则认为是发生了Cnt被Z信号清零事件
        if(        enc.encoder_incremental_cnt < -0.5*    SYSTEM_QEP_QPOSMAX_PLUS_1)
                   enc.encoder_incremental_cnt += (int32) SYSTEM_QEP_QPOSMAX_PLUS_1;
        else if (  enc.encoder_incremental_cnt >  0.5*    SYSTEM_QEP_QPOSMAX_PLUS_1)
                   enc.encoder_incremental_cnt -= (int32) SYSTEM_QEP_QPOSMAX_PLUS_1;

        enc.sum_qepPosCnt       -= enc.MA_qepPosCnt[enc.cursor];
        enc.sum_qepPosCnt       += enc.encoder_incremental_cnt;
        enc.MA_qepPosCnt[enc.cursor] = enc.encoder_incremental_cnt;
        enc.cursor+=1; // 完事以后再加一
        if(enc.cursor>=MA_SEQUENCE_LENGTH){
            enc.cursor=0; // Reset enc.cursor
        }
    enc.rpm = enc.sum_qepPosCnt*SYSTEM_QEP_REV_PER_PULSE * 60 * MA_SEQUENCE_LENGTH_INVERSE * CL_TS_INVERSE;
    enc.omg_elec     = enc.rpm * RPM_2_ELEC_RAD_PER_SEC; // 机械转速（单位：RPM）-> 电气角速度（单位：elec.rad/s)
    enc.theta_d_elec = enc.theta_d__state;

    // Output of the moving average is speed. CTRL.I->rpm = how many counts / time elapsed
    return enc.rpm;
    // return ENC.sum_qepPosCnt*SYSTEM_QEP_REV_PER_PULSE * 60 / (MA_SEQUENCE_LENGTH*CL_TS);
    // return ENC.sum_qepPosCnt*SYSTEM_QEP_REV_PER_PULSE * 6e4; // 6e4 = 60 / (MA_SEQUENCE_LENGTH*CL_TS) 

    #undef enc
}



double difference_between_two_angles(double first, double second){
    while(first>2*M_PI){
        first-=2*M_PI;
    }
    while(second>2*M_PI){
        second-=2*M_PI;
    }

    while(first<0.0){
        first+=2*M_PI;
    }
    while(second<0.0){
        second+=2*M_PI;
    }

    if(fabs(first-second)<M_PI){
        return first-second;
    }else{
        if(first>second){
            return first-2*M_PI-second;
        }else{                
            return first+2*M_PI-second;
        }
    }
}



#if PC_SIMULATION == TRUE
    // 写变量meta-data到文件
    void write_header_to_file(FILE *fw){
        printf("%s\n", DATA_FILE_NAME);

        fprintf(fw, DATA_LABELS);

        {
            // 将除了变量数据和变量名以外的信息写入文件“info.dat”，包括采样时间，降采样倍数，数据文件名。
            FILE *fw2;
            fw2 = fopen("../dat/info.dat", "w");
            fprintf(fw2, "CL_TS,DOWN_SAMPLE,DATA_FILE_NAME\n");
            fprintf(fw2, "%g, %d, %s\n", CL_TS, DOWN_SAMPLE, DATA_FILE_NAME);
            fclose(fw2);
        }
    }
    // 写变量值到文件
    void write_data_to_file(FILE *fw){
        static int bool_animate_on = FALSE;
        static int j=0,jj=0; // j,jj for down sampling

        // if(CTRL.timebase>20)
        {
            if(++j == DOWN_SAMPLE)
            {
                j=0;
                fprintf(fw, DATA_FORMAT, DATA_DETAILS);
            }
        }
    }
#endif


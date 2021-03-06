
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>

#include "utility.h"
#include "PPPPChannel.h"  

#include "IOTCAPIs.h"
#include "IOTCWakeUp.h"
#include "AVAPIs.h"
#include "AVFRAMEINFO.h"
#include "AVIOCTRLDEFs.h"

#include "libvdp.h"
#include "appreq.h"
#include "apprsp.h"

#define ENABLE_AEC

#ifdef PLATFORM_ANDROID
#else
#define ENABLE_AGC
#endif

#define ENABLE_NSX_I
#define ENABLE_NSX_O
//#define ENABLE_VAD
#define ENABLE_AUDIO_RECORD

#ifdef PLATFORM_ANDROID

#include <jni.h>
extern JavaVM * g_JavaVM;

#else

static inline long gettid(){ return (long)pthread_self();}

#endif

extern jobject   g_CallBack_Handle;
extern jmethodID g_CallBack_ConnectionNotify;
extern jmethodID g_CallBack_MessageNotify;
extern jmethodID g_CallBack_VideoDataProcess;
extern jmethodID g_CallBack_AlarmNotifyDoorBell;
extern jmethodID g_CallBack_UILayerNotify;

extern COMMO_LOCK g_CallbackContextLock;

unsigned long GetAudioTime(){
#ifdef PLATFORM_ANDROID
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#else
    struct timeval  ts;
    gettimeofday(&ts,NULL);
    return (ts.tv_sec * 1000 + ts.tv_usec / 1000000);
#endif
}

int avSendIOCtrlEx(
	int nAVChannelID, 
	unsigned int nIOCtrlType, 
	const char *cabIOCtrlData, 
	int nIOCtrlDataSize
){
	while(1){
		int ret = avSendIOCtrl(nAVChannelID, nIOCtrlType, cabIOCtrlData, nIOCtrlDataSize);

		if(ret == AV_ER_SENDIOCTRL_ALREADY_CALLED){
//			Log3("avSendIOCtrl is running by other process.");
			usleep(1000); continue;
		}

		return ret;
	}

	return -1;
}


#ifdef PLATFORM_ANDROID

// this callback handler is called every time a buffer finishes recording
static void recordCallback(
	SLAndroidSimpleBufferQueueItf bq, 
	void *context
){
	OPENXL_STREAM * p = (OPENXL_STREAM *)context;
	CPPPPChannel * hPC = (CPPPPChannel *)p->context;

    short * hFrame = p->recordBuffer+(p->iBufferIndex * hPC->Audio10msLength  / sizeof(short));
	
	hPC->hAudioGetList->Put((char*)hFrame,hPC->Audio10msLength);

	(*p->recorderBufferQueue)->Enqueue(p->recorderBufferQueue,(char*)hFrame,hPC->Audio10msLength);

    p->iBufferIndex = (p->iBufferIndex+1)%CBC_CACHE_NUM;
}

// this callback handler is called every time a buffer finishes playing
static void playerCallback(
	SLAndroidSimpleBufferQueueItf bq, 
	void *context
){
	OPENXL_STREAM * p = (OPENXL_STREAM *)context;
	CPPPPChannel * hPC = (CPPPPChannel *)p->context;

    short *hFrame = p->outputBuffer+(p->oBufferIndex * hPC->Audio10msLength / sizeof(short));

	hPC->hAudioPutList->Put((char*)hFrame,hPC->Audio10msLength);
	
	int stocksize = hPC->hSoundBuffer->Used();

	if(stocksize >= hPC->Audio10msLength){
		hPC->hSoundBuffer->Get((char*)hFrame,hPC->Audio10msLength);
	}else{
        memset((char*)hFrame,0,hPC->Audio10msLength);
	}

	(*p->bqPlayerBufferQueue)->Enqueue(p->bqPlayerBufferQueue,(char*)hFrame,hPC->Audio10msLength);

    p->oBufferIndex = (p->oBufferIndex+1)%CBC_CACHE_NUM;
}

#else

static void recordCallback(char * data, int lens, void *context){
    OPENXL_STREAM * p = (OPENXL_STREAM *)context;
    CPPPPChannel * hPC = (CPPPPChannel *)p->context;

	if(lens > hPC->Audio10msLength){
        Log3("audio record sample is too large:[%d].",lens);
        return;
    }
    
    char * pr = (char*)p->recordBuffer;
    memcpy(pr + p->recordSize,data,lens);
    p->recordSize += lens;
    
    if(p->recordSize >= hPC->Audio10msLength){
        hPC->hAudioGetList->Put(pr,hPC->Audio10msLength);
        p->recordSize -= hPC->Audio10msLength;
        memcpy(pr,pr + hPC->Audio10msLength,p->recordSize);
    }
}

static void playerCallback(char * data, int lens, void *context){
    OPENXL_STREAM * p = (OPENXL_STREAM *)context;
    CPPPPChannel * hPC = (CPPPPChannel *)p->context;

	 if(lens > hPC->Audio10msLength){
        Log3("audio output sample is too large:[%d].",lens);
        return;
    }
    
    int stocksize = hPC->hSoundBuffer->Used();
    
    if(stocksize >= lens){
        hPC->hSoundBuffer->Get((char*)data,lens);
    }else{
        memset((char*)data,0,lens);
    }
    
    char * po = (char*)p->outputBuffer;
    memcpy(po + p->outputSize,data,lens);
    p->outputSize += lens;
    
    if(p->outputSize >= hPC->Audio10msLength){
        hPC->hAudioPutList->Put(po,hPC->Audio10msLength);
        p->outputSize -= hPC->Audio10msLength;
        memcpy(po,
               po + hPC->Audio10msLength,
               p->outputSize);
    }

}

#endif

void CheckServHandler(
	int result, 
	void * userData
){
	int * pErr = (int *)userData;
	*pErr = result;
}

void * IOCmdRecvProcess(
	void * hVoid
){
    SET_THREAD_NAME("IOCmdRecvProcess");

	Log2("current thread id is:[%d].",gettid());

	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;
    JNIEnv * hEnv = NULL;
    
#ifdef PLATFORM_ANDROID
	char isAttached = 0;

	int status = g_JavaVM->GetEnv((void **) &(hEnv), JNI_VERSION_1_4); 
	if(status < 0){ 
		status = g_JavaVM->AttachCurrentThread(&(hEnv), NULL); 
		if(status < 0){
			Log("iocmd recv process AttachCurrentThread Failed!!");
			return NULL;
		}
		isAttached = 1; 
	}
#endif

	unsigned int IOCtrlType = 0;

	int avIdx = hPC->avIdx;
	int jbyteArrayLens = 8192;
	
	char D[2048] = {0};
	char S[8192] = {0};

	CMD_CHANNEL_HEAD * hCCH = (CMD_CHANNEL_HEAD*)D;

	jbyteArray jbyteArray_msg = hEnv->NewByteArray(jbyteArrayLens);

    while(hPC->iocmdRecving){
		int ret = avRecvIOCtrl(avIdx,
			&IOCtrlType,
			 hCCH->d,
			 sizeof(D) - sizeof(CMD_CHANNEL_HEAD),
			 100);

		if(ret < 0){
			switch(ret){
				case AV_ER_DATA_NOREADY:
				case AV_ER_TIMEOUT:
					usleep(1000);
					continue;
			}

			Log3("[X:%s]=====>iocmd get sid:[%d] error:[%d]",hPC->szDID,hPC->SID,ret);
			
			break;
		}

		Log3("[X:%s]=====>iocmd recv frame len:[%d] cmd:[0x%04x].",hPC->szDID,ret,IOCtrlType);

		hCCH->len = ret;
        
        switch(IOCtrlType){
			case IOTYPE_USER_IPCAM_DEL_IOT_RESP:
			case IOTYPE_USER_IPCAM_LST_IOT_RESP:
			case IOTYPE_USER_IPCAM_RAW_RESP: // for byte data response
				hEnv->SetByteArrayRegion(jbyteArray_msg, 0, hCCH->len, (const jbyte *)hCCH->d);
				break;
			case IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL_RESP:{
					SMsgAVIoctrlPlayRecordResp * hRQ = (SMsgAVIoctrlPlayRecordResp *)hCCH->d;
					hPC->playrecChannel = hRQ->result;
					Log3("hPC->playrecChannel:[%d] hRQ->command:[%d]",hPC->playrecChannel,hRQ->command);
				}
				continue;
			case IOTYPE_USER_IPCAM_DEVICESLEEP_RESP:{	
					SMsgAVIoctrlSetDeviceSleepResp * hRQ = (SMsgAVIoctrlSetDeviceSleepResp *)hCCH->d;
					if(hRQ->result == 0){
						Log3("[X:%s]=====>device sleeping now.\n",hPC->szDID);
					}else{
						Log3("[X:%s]=====>device sleeping failed,still keep online.\n",hPC->szDID);
					}
				}
				break;
            case IOTYPE_USER_IPCAM_ALARMING_REQ:{
                SMsgAVIoctrlAlarmingReq * hRQ = (SMsgAVIoctrlAlarmingReq *)hCCH->d;
                
                char sTIME[64] = {0};
                char sTYPE[16] = {0};
                
                const char * wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                struct tm stm;
                struct tm * p = &stm;
                
                sprintf(sTYPE,"%d",hRQ->AlarmType);
                
                p = localtime_r((const time_t *)&hRQ->AlarmTime,&stm); //
                if(p == NULL){
                    time_t t;
                    time(&t);
                    p = localtime_r(&t,&stm);
                }
                
                sprintf(sTIME,
                        "%04d-%02d-%02d-%s-%02d-%02d-%02d",
                        1900+p->tm_year,
                        p->tm_mon + 1,
                        p->tm_mday,
                        wday[p->tm_wday],
                        p->tm_hour,
                        p->tm_min,
                        p->tm_sec);
                
                hPC->AlarmNotifyDoorBell(hEnv,
                        hRQ->AlarmDID,
                        sTYPE,
                        sTIME);
                }
                break;
            default:
				memset(S,0,sizeof(S));
                ParseResponseForUI(IOCtrlType, hCCH->d, S, jbyteArrayLens);
				hCCH->len = strlen(S);
				hEnv->SetByteArrayRegion(jbyteArray_msg, 0, hCCH->len, (const jbyte *)S);
				Log3("response parse to json is:[%s].",S);
                break;
        }
        
        GET_LOCK( &g_CallbackContextLock );	
        
        jstring jstring_did = hEnv->NewStringUTF(hPC->szDID);
        
        hEnv->CallVoidMethod(
			g_CallBack_Handle,
			g_CallBack_UILayerNotify,
			jstring_did,
			IOCtrlType,
			jbyteArray_msg,
			hCCH->len
			);

		hEnv->DeleteLocalRef(jstring_did);
        
        PUT_LOCK( &g_CallbackContextLock );	
        
        Log3("[X:%s]=====>call UILayerNotify done by cmd:[%d].\n",hPC->szDID,IOCtrlType);
        
        memset(D,0,sizeof(D));
    }

	hEnv->DeleteLocalRef(jbyteArray_msg);

#ifdef PLATFORM_ANDROID
	if(isAttached) g_JavaVM->DetachCurrentThread();
#endif
    
	Log3("[X:%s]=====>iocmd recv proc exit.",hPC->szDID);

	return NULL;
}

static void * VideoPlayProcess(
	void * hVoid
){
	SET_THREAD_NAME("VideoPlayProcess");

	Log2("current thread id is:[%d].",gettid());

	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;
	JNIEnv * hEnv = NULL;
    
#ifdef PLATFORM_ANDROID
	char isAttached = 0;

	int status = g_JavaVM->GetEnv((void **) &(hEnv), JNI_VERSION_1_4); 
    if(status < 0){ 
        status = g_JavaVM->AttachCurrentThread(&(hEnv), NULL); 
        if(status < 0){
            return NULL;
        }
        isAttached = 1; 
    }
#endif

	jstring    jstring_did = hEnv->NewStringUTF(hPC->szDID);
	jbyteArray jbyteArray_yuv = hEnv->NewByteArray(hPC->YUVSize);
	jbyte *	   jbyte_yuv = (jbyte *)(hEnv->GetByteArrayElements(jbyteArray_yuv,0));
	
	unsigned int frameTimestamp = 0;

	GET_LOCK(&hPC->DisplayLock);
	hPC->hVideoFrame = NULL;
	PUT_LOCK(&hPC->DisplayLock);

	while(hPC->mediaLinking){

		if(g_CallBack_Handle == NULL || g_CallBack_VideoDataProcess == NULL){
			usleep(1000); continue;
		}


		if(hPC->videoPlaying != 1){
//			Log3("video play process paused...");
			sleep(1); 
			continue;
		}

		GET_LOCK(&hPC->DisplayLock);

		if(hPC->hVideoFrame == NULL){
			PUT_LOCK(&hPC->DisplayLock);
			usleep(1000); continue;
		}
		
		memcpy(jbyte_yuv,hPC->hVideoFrame,hPC->YUVSize);
		frameTimestamp = hPC->FrameTimestamp;

		hPC->hVideoFrame = NULL;
		
		int NW = hPC->MW;
		int NH = hPC->MH;
		int NS = NW * NH * 3 / 2;

		PUT_LOCK(&hPC->DisplayLock);

		/*
		hPC->hDec->YUV420CUTSIZE(
			(char*)jbyte_yuv,
			(char*)jbyte_yuv,
			hPC->MW,
			hPC->MH,
			NW,
			NH
			);
		*/

		// put h264 yuv data to java layer
		GET_LOCK(&g_CallbackContextLock);	

		// for yuv draw process
		hEnv->CallVoidMethod(
			g_CallBack_Handle,
			g_CallBack_VideoDataProcess,
			jstring_did,
			jbyteArray_yuv,
			1, 
			NS,
			NW,
			NH,
			frameTimestamp);

		PUT_LOCK(&g_CallbackContextLock);
	}

	hEnv->ReleaseByteArrayElements(jbyteArray_yuv,jbyte_yuv,0);
	hEnv->DeleteLocalRef(jbyteArray_yuv);
	hEnv->DeleteLocalRef(jstring_did);

#ifdef PLATFORM_ANDROID
	if(isAttached) g_JavaVM->DetachCurrentThread();
#else
    jbyteArray_yuv = NULL;
    jstring_did = NULL;
#endif
	Log3("video play proc exit.");

	return NULL;
}

static void * VideoRecvProcess(void * hVoid){

	SET_THREAD_NAME("VideoRecvProcess");
	
	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;
	
	int 			ret = 0;
	
	FRAMEINFO_t  	frameInfo;
	int 			outBufSize = 0;
	int 			outFrmSize = 0;
	int 			outFrmInfoSize = 0;
	int 			firstKeyFrameComming = 0;
	int				isKeyFrame = 0;

	unsigned int	server = 0;
	int 			resend = 0;
	int 			avIdx = hPC->avIdx;

	int 			FrmSize = hPC->YUVSize/3;
	AV_HEAD * 		hFrm = (AV_HEAD*)malloc(FrmSize);
	char    * 		hYUV = (char*)malloc(hPC->YUVSize);

	if(hFrm == NULL){
		Log3("invalid hFrm ptr address for video frame process.");
		hPC->videoPlaying = 0;
	}

	if(hYUV == NULL){
		Log3("invalid hYUV ptr address for video frame process.");
		hPC->videoPlaying = 0;
	}

	memset(hFrm,0,FrmSize);
	memset(hYUV,0,hPC->YUVSize);

	hFrm->startcode = 0xa815aa55;
	
	memset(&frameInfo,0,sizeof(frameInfo));
	
	while(hPC->mediaLinking)
	{
		if(hPC->videoPlaying == 0){
//            Log3("video recv process paused...");
			avClientCleanVideoBuf(avIdx);
			avIdx = -1;
			sleep(1); 
			continue;
		}

		// set play index
		if(avIdx < 0){
            
            if(hPC->szURL[0]){
                avIdx = hPC->rpIdx;
            }else{
                avIdx = hPC->avIdx;
            }
            
			Log3("video playing start with idx:[%d] mode:[%s]",
				avIdx,
				hPC->rpIdx > 0 ? "replay" : "livestream"
				);
            
			continue;
		}
	
		ret = avRecvFrameData2(
			avIdx, 
			hFrm->d,
			FrmSize - sizeof(AV_HEAD),
			&outBufSize, 
			&outFrmSize, 
			(char *)&frameInfo, 
			sizeof(FRAMEINFO_t), 
			&outFrmInfoSize, 
			&hFrm->frameno);

		if(ret < 0){
			switch(ret){
				case AV_ER_LOSED_THIS_FRAME:
				case AV_ER_INCOMPLETE_FRAME:
					Log3("tutk lost frame with error:[%d].",ret);
					firstKeyFrameComming = 0;
					continue;
				case AV_ER_DATA_NOREADY:
					usleep(10000);
					continue;
				default:
//					Log3("tutk recv frame with error:[%d],avIdx:[%d].",ret,hPC->avIdx);
					break;
			}
			continue;
		}

		if(frameInfo.flags == IPC_FRAME_FLAG_IFRAME){
			firstKeyFrameComming = 1;
			isKeyFrame = 1;
		}else{
			isKeyFrame = 0;
		}

		if(firstKeyFrameComming != 1){
			Log3("waiting for first video key frame coming.\n");
			continue;
		}

		hFrm->type = frameInfo.flags;
		hFrm->len = ret;

		int W = 0;
		int H = 0;

		// decode h264 frame
		if(hPC->hDec->DecoderFrame((uint8_t *)hFrm->d,hFrm->len,W,H,isKeyFrame) <= 0){
			Log3("decode h.264 frame failed.");
			firstKeyFrameComming = 0;
			continue;
		}

		if(W <= 0 || H <= 0){
			Log3("invalid decode resolution W:%d H:%d.",W,H);
			continue;
		}

		int nBytesHave = hPC->hVideoBuffer->Available();

		if(hPC->recordingExit){
			if(nBytesHave >= hFrm->len + sizeof(AV_HEAD)){
				hPC->hVideoBuffer->Put((char*)hFrm,hFrm->len + sizeof(AV_HEAD));
			}
		}

		if(frameInfo.last == 1){
			GET_LOCK(&hPC->DisplayLock);
		}else{
			if(TRY_LOCK(&hPC->DisplayLock) != 0){
				continue;
			}
		}

		hPC->MW = W - hPC->MWCropSize;
		hPC->MH = H - hPC->MHCropSize;
		
		// Get h264 yuv data
		hPC->hDec->GetYUVBuffer((uint8_t*)hYUV,hPC->YUVSize,hPC->MW,hPC->MH);
		hPC->hVideoFrame = hYUV;
		hPC->FrameTimestamp = frameInfo.timestamp;

		PUT_LOCK(&hPC->DisplayLock);
    }

	GET_LOCK(&hPC->DisplayLock);
    
	if(hFrm)   free(hFrm); hFrm = NULL;
	if(hYUV)   free(hYUV); hYUV = NULL;
    
    hPC->hVideoFrame = NULL;
    
	PUT_LOCK(&hPC->DisplayLock);
    
    return NULL;
}

static void * AudioRecvProcess(
	void * hVoid
){    
	SET_THREAD_NAME("AudioRecvProcess");

	int ret = 0;
    
	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;

	FRAMEINFO_t  frameInfo;
	memset(&frameInfo,0,sizeof(frameInfo));

	int avIdx = hPC->avIdx;

	char Cache[2048] = {0};
	char Codec[4096] = {0};
    
    int  CodecLength = 0;
    int  CodecLengthNeed = 960;
    
	AV_HEAD * hAV = (AV_HEAD*)Cache;

	void * hAgc = NULL;
	void * hNsx = NULL;

	Log3("audio recv process sid:[%d].",hPC->SID);

jump_rst:
	
	if(hPC->mediaLinking == 0) return NULL;
	
	void * hCodec = audio_dec_init(
		hPC->AudioRecvFormat,
		hPC->AudioSampleRate,
		hPC->AudioChannel
		);
	
	if(hCodec == NULL){
//		Log3("initialize audio decodec handle failed.\n");
		sleep(1);
		goto jump_rst;
	}

	hPC->hAudioBuffer->Clear();
	hPC->hSoundBuffer->Clear();

#ifdef ENABLE_AGC
	hAgc = audio_agc_init(
		20,
		2,
		0,
		255,
		hPC->AudioSampleRate);

	if(hAgc == NULL){
		Log3("initialize audio agc failed.\n");
		goto jumperr;
	}
#endif

#ifdef ENABLE_NSX_I
	hNsx = audio_nsx_init(2,hPC->AudioSampleRate);

	if(hNsx == NULL){
		Log3("initialize audio nsx failed.\n");
		goto jumperr;
	}
#endif
	
	while(hPC->mediaLinking){

		if(hPC->audioPlaying == 0){
//            Log3("audio recv process paused...");
			avClientCleanAudioBuf(avIdx);
			hPC->hAudioBuffer->Clear();
			hPC->hSoundBuffer->Clear();
			
			CodecLength = 0;
			avIdx = -1;

			// release audio decode
			audio_dec_free(hCodec);
			hCodec = NULL;
			
			sleep(1); 
			continue;
		}

		// set play index
		if(avIdx < 0){
            
            if(hPC->szURL[0]){
                avIdx = hPC->rpIdx;
            }else{
                avIdx = hPC->avIdx;
            }
            
			Log3("audio playing start with idx:[%d] mode:[%s]",
				avIdx,
				hPC->rpIdx > 0 ? "replay" : "livestream"
				);
            
			continue;
		}

		if(avCheckAudioBuf(avIdx) < 5){
			usleep(10000);
			continue;
		}else if(avCheckAudioBuf(avIdx) > 20){
			avClientCleanAudioBuf(avIdx);
		}
		
		ret = avRecvAudioData(
			avIdx, 
			hAV->d, 
			sizeof(Cache) - sizeof(AV_HEAD), 
			(char *)&frameInfo, 
			sizeof(FRAMEINFO_t), 
			&hAV->frameno
			);

		hAV->len = ret;

		if(ret < 0){
            switch(ret){
				case AV_ER_LOSED_THIS_FRAME:
				case AV_ER_DATA_NOREADY:
					usleep(10000);
					continue;
				default:
//					Log3("tutk recv frame with error:[%d],avIdx:[%d].",ret,hPC->avIdx);
					break;
			}
			continue;
		}

//		LogX("current audio decode id:[%02X].",frameInfo.codec_id);

		if(frameInfo.codec_id != hPC->AudioRecvFormat || hCodec == NULL){
			LogX("invalid packet format for audio decoder:[%02X].",frameInfo.codec_id);
			audio_dec_free(hCodec);
			
			LogX("initialize new audio decoder here.\n")
				
			hCodec = audio_dec_init(
				frameInfo.codec_id,
				hPC->AudioSampleRate,
				hPC->AudioChannel
				);
			
			if(hCodec == NULL){
				LogX("initialize audio decodec handle for codec:[%02X] failed.",frameInfo.codec_id);
				continue;
			}
			
			LogX("initialize new audio decoder done.\n")
			hPC->AudioRecvFormat = frameInfo.codec_id;
			continue;
		}
		
		if((ret = audio_dec_process(
                hCodec,
                hAV->d,
                hAV->len,
                &Codec[CodecLength],
                sizeof(Codec) - CodecLength)) < 0){
			
			Log3("audio decodec process run error:%d with codec:[%02X] lens:[%d].\n",
				ret,
				hPC->AudioRecvFormat,
				hAV->len
				);
			
			continue;
		}
        
        CodecLength += ret;
        
        if(CodecLength < CodecLengthNeed){
            continue;
        }

		int Round = CodecLengthNeed/hPC->Audio10msLength;
        
		for(int i = 0; i < Round; i++){
#ifdef ENABLE_NSX_I
			audio_nsx_proc(hNsx,&Codec[hPC->Audio10msLength*i],hPC->Audio10msLength);
#endif
#ifdef ENABLE_AGC
			audio_agc_proc(hAgc,&Codec[hPC->Audio10msLength*i],hPC->Audio10msLength);
#endif

			if(hPC->audioEnabled){
				hPC->hSoundBuffer->Put((char*)&Codec[hPC->Audio10msLength*i],hPC->Audio10msLength); // for audio player callback
			}

#ifdef ENABLE_DEBUG
			if(hPC->hRecordAudioRecv > 0){
				int bytes = write(hPC->hRecordAudioRecv,(char*)&Codec[hPC->Audio10msLength*i],hPC->Audio10msLength);
				if(bytes != hPC->Audio10msLength){
					LogX("debug wirte audio recv pcm raw data failed.");
				}
			}
#endif
#ifdef ENABLE_AUDIO_RECORD
			hPC->hAudioBuffer->Put((char*)&Codec[hPC->Audio10msLength*i],hPC->Audio10msLength); // for audio avi record
#endif
		}
        
        CodecLength -= CodecLengthNeed;
        memcpy(Codec,&Codec[CodecLengthNeed],CodecLength);

//		hPC->hAudioBuffer->Write(Codec,ret); // for audio avi record
//		hPC->hSoundBuffer->Write(Codec,ret); // for audio player callback
	}

#ifdef ENABLE_AGC
	audio_agc_free(hAgc);
#endif
#ifdef ENABLE_NSX_I
	audio_nsx_free(hNsx);
#endif

jumperr:

	audio_dec_free(hCodec);

	Log3("audio recv proc exit.");

	return NULL;
}

static void * AudioSendProcess(
	void * hVoid
){
	SET_THREAD_NAME("AudioSendProcess");

	Log2("current thread id is:[%d].",gettid());

	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;
	int ret = 0;

	void * hCodec = NULL;
	void * hAEC = NULL;
	void * hNsx = NULL;
	void * hVad = NULL;
	OPENXL_STREAM * hOSL = NULL;

	SMsgAVIoctrlAVStream ioMsg;	
	memset(&ioMsg,0,sizeof(ioMsg));

	Log3("audio send process sid:[%d].", hPC->SID);

#ifdef ENABLE_DEBUG
	FILE * hOut = fopen("/mnt/sdcard/vdp-output.raw","wb");
	if(hOut == NULL){
		Log3("initialize audio output file failed.\n");
	}
#endif

jump_rst:
	if(hPC->mediaLinking == 0) return NULL;

	hCodec = audio_enc_init(hPC->AudioSendFormat,hPC->AudioSampleRate,hPC->AudioChannel);
	if(hCodec == NULL){
//		Log3("initialize audio encodec handle failed.\n");
		sleep(1);
		goto jump_rst;
	}

#ifdef ENABLE_AEC
	hAEC = audio_echo_cancellation_init(3,hPC->AudioSampleRate);
	if(hAEC == NULL){
		Log3("initialize audio aec failed.\n");
	}
#endif

#ifdef ENABLE_NSX_O
	hNsx = audio_nsx_init(2,hPC->AudioSampleRate);
	if(hNsx == NULL){
		Log3("initialize audio nsx failed.\n");
	}
#endif

#ifdef ENABLE_VAD
	hVad = audio_vad_init();
	if(hVad == NULL){
		Log3("initialize audio vad failed.\n");
	}
#endif

wait_next:
	
	if(hPC->mediaLinking == 0) return NULL;

	if(hPC->spIdx < 0){
//		Log3("tutk start audio send process with error:[%d] try again.",hPC->spIdx);
		sleep(1);
		goto wait_next;
	}
	
	Log3("tutk start audio send process by speaker channel:[%d] spIdx:[%d].",
		hPC->speakerChannel,
		hPC->spIdx);

	char hFrame[2*960] = {0};
	char hCodecFrame[2*960] = {0};

	AV_HEAD * hAV = (AV_HEAD*)hFrame;
	char * WritePtr = hAV->d;

	int nBytesNeed = 960;	// max is 960 for opus encoder process.
	int nVadFrames = 0;

	char speakerData[320] = {0};
	char captureData[320] = {0};

	while(hPC->mediaLinking){

		if(hPC->audioPlaying == 0){
			goto wait_next;	// wait next audio receiver from device
		}

		int captureLens = hPC->hAudioGetList->Used();
		int speakerLens = hPC->hAudioPutList->Used();

		if(captureLens < hPC->Audio10msLength 
		|| speakerLens < hPC->Audio10msLength
		){
			usleep(10000);
			continue;
		}

		hPC->hAudioGetList->Get(captureData,hPC->Audio10msLength);
		hPC->hAudioPutList->Get(speakerData,hPC->Audio10msLength);

		if(hPC->voiceEnabled != 1){
			usleep(10000); 
			continue;
		}

#ifdef ENABLE_AEC
		if (audio_echo_cancellation_farend(hAEC,(char*)speakerData,hPC->Audio10msLength/sizeof(short)) != 0){
			Log3("WebRtcAecm_BufferFarend() failed.");
		}
		
		if (audio_echo_cancellation_proc(hAEC,(char*)captureData,(char*)WritePtr,hPC->Audio10msLength/sizeof(short)) != 0){
			Log3("WebRtcAecm_Process() failed.");
		}
#else
		memcpy(WritePtr,captureData,hPC->Audio10msLength);
#endif

#ifdef ENABLE_NSX_O
		audio_nsx_proc(hNsx,WritePtr,hPC->Audio10msLength);
#endif

#ifdef ENABLE_VAD
		int logration = audio_vad_proc(hVad,WritePtr,hPC->Audio10msLength);

        if(logration < 1024){
//			Log3("audio detect vad actived:[%d].\n",logration);
			nVadFrames ++;
		}else{
			nVadFrames = 0;
		}
#endif

		hAV->len += hPC->Audio10msLength;
		WritePtr += hPC->Audio10msLength;

		if(hAV->len < nBytesNeed){
			continue;
		}

#ifdef ENABLE_DEBUG
		fwrite(hAV->d,hAV->len,1,hOut);
#endif

#ifdef ENABLE_VAD
		if(nVadFrames > 300){
//			Log3("audio detect vad actived.\n");
            hAV->len = 0;
            WritePtr = hAV->d;
            continue;
		}
#endif
		ret = audio_enc_process(
            hCodec,
            hAV->d,
            hAV->len,
            hCodecFrame,
            sizeof(hCodecFrame));
        
		if(ret < 2){
			Log3("audio encode failed with error:%d.\n",ret);
			hAV->len = 0;
			WritePtr = hAV->d;
			continue;
		}
		
//		struct timeval tv = {0,0};
//		gettimeofday(&tv,NULL);

		FRAMEINFO_t frameInfo;
		memset(&frameInfo, 0, sizeof(frameInfo));
		frameInfo.codec_id = hPC->AudioSendFormat;
		frameInfo.flags = (AUDIO_SAMPLE_8K << 2) | (AUDIO_DATABITS_16 << 1) | AUDIO_CHANNEL_MONO;
//		frameInfo.timestamp = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
        frameInfo.length = ret;

		ret = avSendAudioData(hPC->spIdx,hCodecFrame,ret,&frameInfo,sizeof(FRAMEINFO_t));

		switch(ret){
			case AV_ER_NoERROR:
				break;
			case AV_ER_EXCEED_MAX_SIZE:
//				avServResetBuffer(hPC->spIdx,RESET_AUDIO,0);
				Log3("tutk av server send audio buffer full.");
				break;
			default:
				Log3("tutk av server send audio data failed.err:[%d].", ret);
				break;
		}
	
		hAV->len = 0;
		WritePtr = hAV->d;
	}

	audio_enc_free(hCodec);
#ifdef ENABLE_AEC
	audio_echo_cancellation_free(hAEC);
#endif
#ifdef ENABLE_NSX_O
	audio_nsx_free(hNsx);
#endif
#ifdef ENABLE_VAD
	audio_vad_free(hVad);
#endif
#ifdef ENABLE_DEBUG
	if(hOut) fclose(hOut); hOut = NULL;
#endif

	Log3("audio send proc exit.");

	return NULL;
}

//
// avi recording process
//
void * RecordingProcess(void * Ptr){
	SET_THREAD_NAME("RecordingProcess");

	Log2("current thread id is:[%d].",gettid());

	CPPPPChannel * hPC = (CPPPPChannel*)Ptr;
	if(hPC == NULL){
		Log2("Invalid channel class object.");
		return NULL;
	}

	long long   ts = 0;

	int nFrame = 0;
	int nBytesRead = 0;

	int firstKeyFrameComming = 0;
	int sts = time(NULL);
	int pts = 0;
	int fps = 0;
	int fix = 0;
//	int	isKeyFrame = 0;
	
	hPC->hAudioBuffer->Clear();
	hPC->hVideoBuffer->Clear();

	AV_HEAD * pVFrm = (AV_HEAD*)malloc(hPC->YUVSize/3);
	AV_HEAD * pAFrm = (AV_HEAD*)malloc(hPC->AudioSaveLength + sizeof(AV_HEAD));

	pAFrm->len = hPC->AudioSaveLength;

	while(hPC->recordingExit){

		int Type = WriteFrameType();

		if(Type < 0){
			usleep(10); continue;
		}
		
		if(Type){
			int vBytesHave = hPC->hVideoBuffer->Used();
			
			if(vBytesHave > (int)(sizeof(AV_HEAD))){
				nBytesRead = hPC->hVideoBuffer->Get((char*)pVFrm,sizeof(AV_HEAD));

				if(pVFrm->startcode != 0xa815aa55){
					Log3("invalid video frame lens:[%d].",pVFrm->len);
					hPC->hVideoBuffer->Clear();
					usleep(10); continue;
				}

				if(pVFrm->type == IPC_FRAME_FLAG_IFRAME){
					firstKeyFrameComming = 1;
				}

				if(firstKeyFrameComming != 1){
					hPC->hVideoBuffer->Mov(pVFrm->len);
					continue;
				}else{
					nBytesRead = hPC->hVideoBuffer->Get(pVFrm->d,pVFrm->len);
				}

				hPC->RecorderWrite(
					pVFrm->d,pVFrm->len,
					1,
					pVFrm->type == IPC_FRAME_FLAG_IFRAME ? 1: 0,
					ts);

				nFrame++;
			}else{
                
                if(pts <= 5) continue;
                if(fix == 0 || firstKeyFrameComming != 1){
                    continue;
                }
			
				Log3("recording fps:[%d] lost frame count:[%d] auto fix.\n",fps,fix);

				for(int i = 0;i < fix;i++){
					hPC->RecorderWrite(
						pVFrm->d,128,
						1,
						0,
						ts
						);
				}

				nFrame += fix;
			}

			pts = time(NULL) - sts;
			pts = pts > 0 ? pts : 1;
			
			fps = nFrame / pts;
			
			fix = hPC->FPS - fps;
			fix = fix > 0 ? fix : 0;
			
		}else{
            if(firstKeyFrameComming != 1){
                continue;
            }

#ifdef ENABLE_AUDIO_RECORD
			int aBytesHave = hPC->hAudioBuffer->Used();
			
			if(aBytesHave > pAFrm->len){
				nBytesRead = hPC->hAudioBuffer->Get(pAFrm->d,pAFrm->len);
				hPC->RecorderWrite(pAFrm->d,nBytesRead,0,0,ts);
			}else{
				memset(pAFrm->d,0,pAFrm->len);
				hPC->RecorderWrite(pAFrm->d,pAFrm->len,0,0,ts);
			}
#endif
		}
	}

	free(pAFrm); pAFrm = NULL;
	free(pVFrm); pVFrm = NULL;

	Log3("stop recording process done.");
	
	return NULL;
}

void   CheckPPPPHandler(int result, void * userData){
	*(int*)userData = result;
}

void * MeidaCoreProcess(
	void * hVoid
){
	SET_THREAD_NAME("MeidaCoreProcess");
	
	LogX("[%d]:current thread id.",gettid());

	CPPPPChannel * hPC = (CPPPPChannel*)hVoid;

	JNIEnv * hEnv = NULL;
	
#ifdef PLATFORM_ANDROID
    char isAttached = 0;

	int err = g_JavaVM->GetEnv((void **) &(hEnv), JNI_VERSION_1_4);
	if(err < 0){
		err = g_JavaVM->AttachCurrentThread(&(hEnv), NULL);
		if(err < 0){
			Log3("iocmd send process AttachCurrentThread Failed!!");
			return NULL;
		}
		isAttached = 1;  
	}
#endif

	hPC->hCoreEnv = hEnv;

	int resend = 0;
    int status = 0;
	int counts = 0;
	int result = 1;
	int Err = 0;

	hPC->startSession = 0;

connect:
	if(hPC->mediaLinking == 0){
		Log3("[0:%s]=====>close connection process by flag.",hPC->szDID);
		goto jumperr;
	}
	
    hPC->MsgNotify(hEnv, MSG_NOTIFY_TYPE_PPPP_STATUS, PPPP_STATUS_CONNECTING);

	result = 1;

	if(hPC->isWakeUp == 0){
		IOTC_Check_Device_On_Line(hPC->szDID,10 * 1000,CheckPPPPHandler,&result);

		counts = 0;
		while(result > 0){
			if((counts % 100) == 0) Log4("waiting for IOTC_Check_Device_On_Line %02d.",counts);
			usleep(10 * 1000);
			counts++;
		}

		Log4("IOTC_Check_Device_On_Line break status is:[%d]",result);

		switch(result){
			case IOTC_ER_TIMEOUT:
			case IOTC_ER_NETWORK_UNREACHABLE: // Network is unreachable, please check the network settings 
			case IOTC_ER_MASTER_NOT_RESPONSE: // IOTC master servers have no response 
			case IOTC_ER_TCP_CONNECT_TO_SERVER_FAILED: // Cannot connect to IOTC servers in TCP
			case IOTC_ER_CAN_NOT_FIND_DEVICE: // IOTC servers cannot locate the specified device
			case IOTC_ER_SERVER_NOT_RESPONSE: // All servers have no response
			case IOTC_ER_TCP_TRAVEL_FAILED: // Cannot connect to masters in neither UDP nor TCP
				status = PPPP_STATUS_CONNECT_FAILED;
			 	hPC->MsgNotify(hEnv, MSG_NOTIFY_TYPE_PPPP_STATUS, status);
				goto jumperr;
			case IOTC_ER_DEVICE_IS_SLEEP:
				status = PPPP_STATUS_DEVICE_SLEEP;
				hPC->MsgNotify(hEnv, MSG_NOTIFY_TYPE_PPPP_STATUS, status);
		//		if(hPC->isWakeUp > 0 && hPC->isWakeUp < 8){ 
		//				Log4("wakeup mode, break for auto wakeup");
		//				break;
		//			Log4("wakeUp mode, check status by IOTC_Check_Device_On_Line again");
		//			hPC->SleepingClose();
		//			goto connect;
		//		}else{
					Log4("wakeup mode, break for auto wakeup");
					goto jumperr;
		//		}
			case IOTC_ER_DEVICE_OFFLINE: // The device is not on line.
				status = PPPP_STATUS_DEVICE_NOT_ON_LINE;
				hPC->MsgNotify(hEnv, MSG_NOTIFY_TYPE_PPPP_STATUS, status);
				goto jumperr;
			default:
				break;
		}
	}

    hPC->playrecChannel = -1;
	hPC->sessionID = IOTC_Get_SessionID();
    
	if(hPC->sessionID < 0){
		LogX("[1:%s]=====>IOTC_Get_SessionID error code [%d]\n",
             hPC->szDID,
             hPC->sessionID);
		goto jumperr;
	}

	hPC->SID = IOTC_Connect_ByUID_Parallel(hPC->szDID,hPC->sessionID);
	hPC->isWakeUp = 0;
	
	if(hPC->SID < 0){
		Log4("[2:%s]=====>start connection failed with error [%d] with device:[%s]\n",
             hPC->szDID, hPC->SID, hPC->szDID);
		
		switch(hPC->SID){
			case IOTC_ER_UNLICENSE:
                status = PPPP_STATUS_INVALID_ID;
				goto jumperr;
			case IOTC_ER_EXCEED_MAX_SESSION:
			case IOTC_ER_DEVICE_EXCEED_MAX_SESSION:
				Log3("[2:%s]=====>got max session error:[%d].",hPC->szDID,hPC->SID);
				status = PPPP_STATUS_EXCEED_SESSION;
				goto jumperr;
            case IOTC_ER_DEVICE_IS_SLEEP:
				Log3("[2:%s]=====>device in sleep mode.",hPC->szDID);
                status = PPPP_STATUS_DEVICE_SLEEP;
                goto jumperr;
			case IOTC_ER_NETWORK_UNREACHABLE:
//			case IOTC_ER_FAIL_CONNECT_SEARCH:
			case IOTC_ER_FAIL_SETUP_RELAY:
            case IOTC_ER_CAN_NOT_FIND_DEVICE:
			case IOTC_ER_DEVICE_OFFLINE:
                Log3("[2:%s]=====>device not online,ask again.",hPC->szDID);
			case IOTC_ER_DEVICE_NOT_LISTENING:
				status = PPPP_STATUS_DEVICE_NOT_ON_LINE;
                IOTC_Session_Close(hPC->sessionID);
				goto jumperr;
			default:
				status = PPPP_STATUS_CONNECT_FAILED;
                IOTC_Session_Close(hPC->sessionID);
				goto connect;
		}
	}

	Log4("[3:%s]=====>start av client service with user:[%s] pass:[%s].\n", hPC->szDID, hPC->szUsr, hPC->szPwd);

	if(strlen(hPC->szUsr) == 0 || strlen(hPC->szPwd) == 0){
		status = PPPP_STATUS_NOT_LOGIN;
		IOTC_Connect_Stop_BySID(hPC->SID);

		Log3("[3:%s]=====>Device can't login by valid user and pass.\n",
             hPC->szDID);
		
		goto jumperr;
	}

	hPC->avIdx = avClientStart2(hPC->SID,
		hPC->szUsr,
		hPC->szPwd, 
		7, 
		&hPC->deviceType, 
		0, 
		&resend
		);
	
	if(hPC->avIdx < 0){
		LogX("[3:%s]=====>avclient start failed:[%d] \n",
             hPC->szDID, hPC->avIdx);
        
        IOTC_Session_Close(hPC->SID);
		
		switch(hPC->avIdx){
            case AV_ER_EXCEED_MAX_CHANNEL:
                status = PPPP_STATUS_EXCEED_SESSION;
                goto jumperr;
			case AV_ER_WRONG_VIEWACCorPWD:
			case AV_ER_NO_PERMISSION:
				status = PPPP_STATUS_INVALID_USER_PWD;
				goto jumperr;
			default:
				break;
		}
	
		goto connect;
	}

	Log3("[4:%s]=====>session:[%d] idx:[%d] did:[%s] resend:[%d].",
        hPC->szDID,
		hPC->SID,
		hPC->avIdx,
		hPC->szDID,
		resend
		);

    hPC->iocmdRecving = 1;
    
	hPC->audioPlaying = 0;
	hPC->videoPlaying = 0;
	
	hPC->audioEnabled = 1;
	hPC->voiceEnabled = 1;

	hPC->MsgNotify(hEnv,MSG_NOTIFY_TYPE_PPPP_STATUS, PPPP_STATUS_ON_LINE);
	
    Err = pthread_create(&hPC->iocmdRecvThread,NULL,IOCmdRecvProcess,(void*)hPC);
	if(Err != 0){
		Log3("create iocmd recv process failed.");
		goto jumperr;
	}

	Err = pthread_create(&hPC->audioRecvThread,NULL,AudioRecvProcess,(void*)hPC);
	if(Err != 0){
		Log3("create audio recv process failed.");
		goto jumperr;
	}

	Err = pthread_create(&hPC->audioSendThread,NULL,AudioSendProcess,(void*)hPC);
	if(Err != 0){
		Log3("create audio send process failed.");
		goto jumperr;
	}

	Err = pthread_create(&hPC->videoRecvThread,NULL,VideoRecvProcess,(void*)hPC);
	if(Err != 0){
		Log3("create video recv process failed.");
		goto jumperr;
	}

	Err = pthread_create(&hPC->videoPlayThread,NULL,VideoPlayProcess,(void*)hPC);
	if(Err != 0){
		Log3("create video play process failed.");
		goto jumperr;
	}

	while(hPC->mediaLinking){
		struct st_SInfo sInfo;
		int ret = IOTC_Session_Check(hPC->SID,&sInfo);
		
		if(ret < 0){
			Log4("IOTC_Session_Check failed with error:[%d]",ret);	
			
			switch(ret){
				case IOTC_ER_DEVICE_OFFLINE:
					status = PPPP_STATUS_DEVICE_NOT_ON_LINE;
					break;
				case IOTC_ER_DEVICE_IS_SLEEP:
					status = PPPP_STATUS_DEVICE_SLEEP;
					break;
				default:
					hPC->mediaLinking = 0;
					hPC->isWakeUp = 0;
					hPC->PPPPClose();
					hPC->CloseWholeThreads();
					hPC->mediaLinking = 1;
					hPC->MsgNotify(hEnv, MSG_NOTIFY_TYPE_PPPP_STATUS, PPPP_STATUS_CONNECTING);
					sleep(5);
					goto connect;
			}
			break;
		}

		if(hPC->startSession){	// for reconnect, we just refresh status for ui layer
//			hPC->MsgNotify(hEnv,MSG_NOTIFY_TYPE_PPPP_STATUS, PPPP_STATUS_ON_LINE);
			hPC->startSession = 0;
		}
		
		sleep(1);
	}

jumperr:	
	
	GET_LOCK(&hPC->DestoryLock);

	hPC->isWakeUp = 0;
    hPC->PPPPClose();
    hPC->CloseWholeThreads(); // make sure other service thread all exit.
    hPC->MsgNotify(hEnv,MSG_NOTIFY_TYPE_PPPP_STATUS,status == 0 ? PPPP_STATUS_CONNECT_FAILED : status);

	Log3("[OPENXL] FREE RESOURCE BY MEDIA CORE PROCESS.");
	if(hPC->hOSL) FreeOpenXLStream(hPC->hOSL);
	hPC->hOSL = NULL;

	PUT_LOCK(&hPC->DestoryLock);
	PUT_LOCK(&hPC->PlayingLock);
	PUT_LOCK(&hPC->SessionLock);
    
    Log4("[%d]:MediaCoreProcess Exit By Status:[%d].",gettid(),status);

#ifdef PLATFORM_ANDROID
	if(isAttached) g_JavaVM->DetachCurrentThread();
#endif

	return NULL;
}

CPPPPChannel::CPPPPChannel(
	char * did, 
	char * usr, 
	char * pwd,
	char * svr,
	char * connectionType
){ 
    memset(szDID, 0, sizeof(szDID));
    strcpy(szDID, did);

    memset(szUsr, 0, sizeof(szUsr));
    strcpy(szUsr, usr);

    memset(szPwd, 0, sizeof(szPwd));
    strcpy(szPwd, pwd);    

    memset(szSvr, 0, sizeof(szSvr));
    strcpy(szSvr, svr);
    
    memset(szURL, 0, sizeof(szURL));
    
    iocmdRecving = 0;
    videoPlaying = 0;
	audioPlaying = 0;

	voiceEnabled = 0;
	audioEnabled = 0;
	speakEnabled = 1;

	isWakeUp = 0;
    
	mediaCoreThread = (pthread_t)-1;
	iocmdRecvThread = (pthread_t)-1;
	videoPlayThread = (pthread_t)-1;
	videoRecvThread = (pthread_t)-1;
	audioSendThread = (pthread_t)-1;
	audioRecvThread = (pthread_t)-1;
	recordingThread = (pthread_t)-1;

	deviceType = -1;
	connectionStatus = PPPP_STATUS_CONNECTING;

	recordingExit = 0;
	avIdx = spIdx = rpIdx = sessionID = -1;

	startSession = 0;

    SID = -1;
    FPS = 25;

	hRecordFile = NULL;

	AudioSaveLength = 0;
	Audio10msLength = 160;

	MW = 1920;
	MH = 1080;
	
	MWCropSize = 0;
	MHCropSize = 0;
	
	YUVSize = (MW * MH) + (MW * MH)/2;

	hAudioBuffer = new CCircleBuffer(   8 * 1024);
	hSoundBuffer = new CCircleBuffer(   8 * 1024);
	hVideoBuffer = new CCircleBuffer(4096 * 1024);

	hAudioPutList = new CCircleBuffer(8 * 1024);
	hAudioGetList = new CCircleBuffer(8 * 1024);
	hOSL = NULL;

#ifdef ENABLE_DEBUG
	hRecordAudioRecv = -1;
	hRecordAudioSend = -1;
#endif

//	hVideoBuffer->Debug(1);
    
    hDec = new CH264Decoder();

	INT_LOCK(&DisplayLock);
	INT_LOCK(&SessionLock);
	INT_LOCK(&CaptureLock);
	
	INT_LOCK(&PlayingLock);
	INT_LOCK(&DestoryLock);
	
}

CPPPPChannel::~CPPPPChannel()
{
    Log3("start free class pppp channel:[0] start.");
    Log3("start free class pppp channel:[1] close p2p connection and threads.");
    
    Close();  
    
    Log3("start free class pppp channel:[2] free buffer.");

	delete(hAudioBuffer);
	delete(hVideoBuffer);
	delete(hSoundBuffer);
	
	delete(hAudioPutList);
	delete(hAudioGetList);

//	hIOCmdBuffer = 
//	hAudioBuffer = 
//	hSoundBuffer = 
//	hVideoBuffer = NULL;
    
    Log3("start free class pppp channel:[3] free lock.");

	DEL_LOCK(&DisplayLock);
	DEL_LOCK(&CaptureLock);
	DEL_LOCK(&SessionLock);
	DEL_LOCK(&PlayingLock);
	DEL_LOCK(&DestoryLock);
    
    if(hDec) delete(hDec); hDec = NULL;
    
    Log3("start free class pppp channel:[4] close done.");
}

void CPPPPChannel::MsgNotify(
    JNIEnv * hEnv,
    int MsgType,
    int Param
){
    GET_LOCK( &g_CallbackContextLock );

	connectionStatus = Param;

    if(g_CallBack_Handle != NULL && g_CallBack_ConnectionNotify!= NULL){
        jstring jsDID = ((JNIEnv *)hEnv)->NewStringUTF(szDID);
        ((JNIEnv *)hEnv)->CallVoidMethod(g_CallBack_Handle, g_CallBack_ConnectionNotify, jsDID, MsgType, Param);
        ((JNIEnv *)hEnv)->DeleteLocalRef(jsDID);
    }

	PUT_LOCK( &g_CallbackContextLock );
}

int CPPPPChannel::PPPPClose()
{
	Log3("close connection by did:[%s] called.",szDID);
    
    avClientStop(avIdx); // free channel
    
	if(SID >= 0){
		avServExit(SID,speakerChannel);	// for avServStart block
		IOTC_Session_Close(SID); 		// close client session handle
	}
	
	avIdx = -1;
	spIdx = -1;
	sessionID = -1;
	SID = -1;

	return 0;
}

int CPPPPChannel::Start(char * usr,char * pwd,char * svr)
{   
	int statusGetTimes = 200;
	int ret = -1;

	if(TRY_LOCK(&SessionLock) != 0){
		Log3("start pppp connection with uuid:[%s] still running",szDID);
		startSession = 1;
		goto check_connection;
	}

	memset(szUsr, 0, sizeof(szUsr));
    strcpy(szUsr, usr);

    memset(szPwd, 0, sizeof(szPwd));
    strcpy(szPwd, pwd);    

    memset(szSvr, 0, sizeof(szSvr));
    strcpy(szSvr, svr);

	mediaLinking = 1;

	LogX("[%d]:start pppp connection to device with uuid:[%s].",gettid(),szDID);
	ret = pthread_create(&mediaCoreThread,NULL,MeidaCoreProcess,(void*)this);
	if(ret != 0){
		Log3("start pppp connection create thread failed.");
		PUT_LOCK(&SessionLock);
		return -1;
	}

check_connection:

	while(statusGetTimes--){
		GET_LOCK( &g_CallbackContextLock );
		int status = connectionStatus;
		PUT_LOCK( &g_CallbackContextLock );

		switch(status){
			case PPPP_STATUS_DEVICE_SLEEP:
				if(isWakeUp == 0){
					Log4("start pppp connection failed with sleep mode.");
					return status;
				}
				usleep(100 * 1000);
				continue;
			case PPPP_STATUS_CONNECTING:
//				Log3("start pppp connection block, status not change.");
				usleep(100 * 1000); // check connection every 100ms
				continue;
			case PPPP_STATUS_ON_LINE:
				Log4("start pppp connection success.");
				return  0;
			default:
				Log4("start pppp connection error:[%d].",status);
				return status;
		}
	}
    
    IOTC_Connect_Stop_BySID(sessionID);
	
    return -1;
}

void CPPPPChannel::Close()
{
	mediaLinking = 0;

	while(1){
		if(TRY_LOCK(&SessionLock) == 0){
			break;
		}
        
        if(sessionID >= 0) IOTC_Connect_Stop_BySID(sessionID);
        if(SID >= 0) avClientExit(SID,0);
        
        mediaLinking = 0;
		
		Log3("waiting for core media process exit.");
		sleep(1);
	}

	PUT_LOCK(&SessionLock);
}

int CPPPPChannel::SleepingStart(){	
	char avMsg[128] = {0};
	int  avErr = 0;
	
	SMsgAVIoctrlAVStream * pMsg = (SMsgAVIoctrlAVStream *)avMsg;

	avErr = IOCmdSend(
		IOTYPE_USER_IPCAM_DEVICESLEEP_REQ,
		avMsg,
		sizeof(SMsgAVIoctrlAVStream),1
		);
	
	if(avErr < 0){
		Log3("avSendIOCtrl failed with err:[%d],avIdx:[%d].",avErr,avIdx);
		return -1;
	}
	
	isWakeUp = 0;
	
	return 0;
}

int CPPPPChannel::SleepingClose(){
	IOTC_WakeUp_Setup_Auto_WakeUp(1);
	isWakeUp = 1;
//	IOTC_WakeUp_WakeDevice(szDID);
	Log4("start pppp connection by wakeup.");
    return 0;
}

int CPPPPChannel::LiveplayStart(){
	char avMsg[1024] = {0};
	int  avErr = 0;

	if(szURL[0]){
		SMsgAVIoctrlPlayRecord * pMsg = (SMsgAVIoctrlPlayRecord *)avMsg;

        pMsg->channel = 0;
		pMsg->command = AVIOCTRL_RECORD_PLAY_START;
        pMsg->stTimeDay.wday = 0;

		sscanf(szURL,"%d-%d-%d %d:%d:%d",
			(int*)&pMsg->stTimeDay.year,
			(int*)&pMsg->stTimeDay.month,
			(int*)&pMsg->stTimeDay.day,
			(int*)&pMsg->stTimeDay.hour,
			(int*)&pMsg->stTimeDay.minute,
			(int*)&pMsg->stTimeDay.second
		);

        Log3("start replay by time:[%d-%d-%d %d:%d:%d].",
             pMsg->stTimeDay.year,
             pMsg->stTimeDay.month,
             pMsg->stTimeDay.day,
             pMsg->stTimeDay.hour,
             pMsg->stTimeDay.minute,
             pMsg->stTimeDay.second);
        
        playrecChannel = 0x3721;

		avErr = IOCmdSend(
			IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL,
			avMsg,
			sizeof(SMsgAVIoctrlPlayRecord),1
			);

		if(avErr < AV_ER_NoERROR){
			Log3("avSendIOCtrl failed with err:[%d],sid:[%d],avIdx:[%d].",avErr,SID,avIdx);
			return -1;
		}

		int times = 10;
		while(times-- && playrecChannel == 0x3721){
			Log3("waiting for replay channel.");
			sleep(1);
		}

		if(playrecChannel <= 0){
			Log3("get replay channel failed with error:[%d].",playrecChannel);
			return -1;
		}
        
		unsigned int svrType = 0;
		int svrRsnd = 1;

		rpIdx = avClientStart2(SID, szUsr, szPwd, 5, &svrType, playrecChannel, &svrRsnd);
        if(rpIdx < 0){
            Log3("avClientStart2 for replay failed:[%d].",rpIdx);
            return -1;
		}
	}else{
		avErr = IOCmdSend(
			IOTYPE_USER_IPCAM_START,
			avMsg,
			sizeof(SMsgAVIoctrlAVStream),1
			);

		if(avErr < AV_ER_NoERROR){
			Log3("avSendIOCtrl failed with err:[%d],sid:[%d],avIdx:[%d].", avErr, SID, avIdx);
			return -1;
		}
	}
	
	return 0;
}

int CPPPPChannel::LiveplayClose(){
	char avMsg[1024] = {0};
	int  avErr = 0;

	if(szURL[0]){
		SMsgAVIoctrlPlayRecord * pMsg = (SMsgAVIoctrlPlayRecord *)avMsg;
		pMsg->command = AVIOCTRL_RECORD_PLAY_STOP;

		avErr = IOCmdSend(
			IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL,
			avMsg,
			sizeof(SMsgAVIoctrlPlayRecord),1);

		avClientStop(rpIdx); 
		rpIdx = -1;
		playrecChannel = -1;
        szURL[0] = 0;
		
	}else{
		avErr = IOCmdSend(
			IOTYPE_USER_IPCAM_STOP,
			avMsg,
			sizeof(SMsgAVIoctrlAVStream),1);
	}

	if(avErr < 0){
		Log3("avSendIOCtrl failed with err:[%d],avIdx:[%d].",avErr,avIdx);
		return -1;
	}

	return 0;
}

int CPPPPChannel::SpeakingStart(){
    
    if(hOSL == NULL){
        hOSL = InitOpenXLStream(
                                AudioSampleRate,
                                AudioChannel,
                                AudioChannel,
                                this,
                                recordCallback,
                                playerCallback
                                );
        if(!hOSL){
            Log3("opensl init failed.");
        }
    }
	
	char avMsg[128] = {0};
	SMsgAVIoctrlAVStream * pMsg = (SMsgAVIoctrlAVStream *)avMsg;

	int avErr = IOCmdSend(
			IOTYPE_USER_IPCAM_AUDIOSTART,
			avMsg,
			sizeof(SMsgAVIoctrlAVStream),1
			);

	if(avErr < AV_ER_NoERROR){
		Log3("avSendIOCtrl failed with err:[%d],sid:[%d],avIdx:[%d].",avErr,SID,avIdx);
		return -1;
	}

	speakEnabled = 1;

#ifdef ENABLE_DEBUG
	char audioRecvPath[128] = {0};
	sprintf(audioRecvPath,"/mnt/sdcard/%d.pcm",time(NULL));
	hRecordAudioRecv = open(audioRecvPath,O_CREAT|O_WRONLY,0755);
	if(hRecordAudioRecv < 0){
		LogX("CREATE AUDIO RECV RECORD FAIL:[%s]",audioRecvPath);
	}else{
		LogX("CREATE AUDIO RECV RECORD DONE:[%s]",audioRecvPath);
	}
#endif

	return 0;
}

int CPPPPChannel::SpeakingClose(){
	
	char avMsg[128] = {0};
	
	int avErr = IOCmdSend(
		IOTYPE_USER_IPCAM_AUDIOSTOP,
		avMsg,
		sizeof(SMsgAVIoctrlAVStream),1);

	if(avErr < 0){
		LogX("avSendIOCtrl failed with err:[%d],avIdx:[%d].",avErr,avIdx);
		return -1;
	}

	speakEnabled = 0;

#ifdef ENABLE_DEBUG
	if(hRecordAudioRecv > 0){
		close(hRecordAudioRecv);
		hRecordAudioRecv = -1;
	}
#endif

	return 0;
}

int CPPPPChannel::MicphoneStart(){
	if(spIdx >= 0){
		Log3("tutk audio send server already start.");
		return 0;
	}
	
	speakerChannel = IOTC_Session_Get_Free_Channel(SID);
	
    if(speakerChannel < 0){
        Log3("tutk get channel for audio send failed:[%d].",speakerChannel);
        return -1;
    }

	char avMsg[128] = {0};
	SMsgAVIoctrlAVStream * pMsg = (SMsgAVIoctrlAVStream *)avMsg;
    
	pMsg->channel = speakerChannel;

	int avErr = IOCmdSend(
		IOTYPE_USER_IPCAM_SPEAKERSTART,
		avMsg,
		sizeof(SMsgAVIoctrlAVStream),1);
	
	if(avErr < AV_ER_NoERROR){
		Log3("avSendIOCtrl failed with err:[%d],sid:[%d],avIdx:[%d].",avErr,SID,avIdx);
		return -1;
	}
    
    Log3("tutk start audio send process by speaker channel:[%d].",speakerChannel);
    spIdx = avServStart(SID, NULL, NULL,  3, 0, speakerChannel);
    if(spIdx < 0){
		Log3("tutk start audio send server failed with error:[%d].",spIdx);
        return -1;
    }
    
    if(hOSL == NULL){
        hOSL = InitOpenXLStream(
                                AudioSampleRate,
                                AudioChannel,
                                AudioChannel,
                                this,
                                recordCallback,
                                playerCallback
                                );
        if(!hOSL){
            Log3("opensl init failed.");
        }
    }

	voiceEnabled = 1;

	return 0;
}

int CPPPPChannel::MicphoneClose(){
	char avMsg[128] = {0};
	SMsgAVIoctrlAVStream * pMsg = (SMsgAVIoctrlAVStream *)avMsg;
	
	pMsg->channel = speakerChannel;

	IOCmdSend(
		IOTYPE_USER_IPCAM_SPEAKERSTOP,
		avMsg,
		sizeof(SMsgAVIoctrlAVStream),1);

	if(spIdx > 0) avServStop(spIdx);
	if(speakerChannel > 0) avServExit(SID,speakerChannel);
	
	voiceEnabled = 0;
	spIdx = -1;

	return 0;
}

int CPPPPChannel::CloseWholeThreads()
{
    iocmdRecving = 0;
    audioPlaying = 0;
	videoPlaying = 0;
	
	recordingExit = 0;

	Log3("stop iocmd process.");
	if(iocmdRecvThread != (pthread_t)-1) pthread_join(iocmdRecvThread,NULL);

	Log3("stop video process.");
    if(videoRecvThread != (pthread_t)-1) pthread_join(videoRecvThread,NULL);
	if(videoPlayThread != (pthread_t)-1) pthread_join(videoPlayThread,NULL);

	Log3("stop audio process.");
    if(audioRecvThread != (pthread_t)-1) pthread_join(audioRecvThread,NULL);
  	if(audioSendThread != (pthread_t)-1) pthread_join(audioSendThread,NULL);

	Log3("stop recording process.");
	RecorderClose();

	iocmdRecvThread = (pthread_t)-1;
	videoRecvThread = (pthread_t)-1;
	videoPlayThread = (pthread_t)-1;
	audioRecvThread = (pthread_t)-1;
	audioSendThread = (pthread_t)-1;

	Log3("stop media thread done.");

	return 0;

}

int CPPPPChannel::CloseMediaStreams(
){
	if(TRY_LOCK(&PlayingLock) == 0){
		Log3("CloseMediaStreams:[stream not in playing.]");
		PUT_LOCK(&PlayingLock);
		return -1;
	}
    
    if(TRY_LOCK(&DestoryLock) != 0){
        Log3("CloseMediaStreams:[media stream will be destory.]");
        PUT_LOCK(&PlayingLock);
        return -1;
    }

	LiveplayClose(); // 关闭视频发送
	MicphoneClose(); // 关闭音频发送
	SpeakingClose(); // 关闭音频接收
	RecorderClose(); // 关闭本地录像
	
	SleepingStart(); // 打开设备休眠
	
	videoPlaying = 0;
	audioPlaying = 0;

	Log3("[OPENXL] FREE RESOURCE BY CLOSE MEDIA STREAM.");
	if(hOSL) FreeOpenXLStream(hOSL);
	hOSL = NULL;

	if(playrecChannel >= 0){
		Log3("close replay client.");
		avClientExit(SID,playrecChannel);
	}
    
	if(spIdx >= 0){
		Log3("close audio send server with idx:[%d]",spIdx);
		avServStop(spIdx);
	}
	
	if(speakerChannel >= 0){
		Log3("shutdown audio send server with sid:[%d] channel:[%d]",SID,speakerChannel);
		avServExit(SID,speakerChannel);
	}

	spIdx = -1;

	Log3("close media stream success ... ");

	PUT_LOCK(&PlayingLock);
	PUT_LOCK(&DestoryLock);

	return 0;
}
	
int CPPPPChannel::StartMediaStreams(
	const char * url,
	int audio_sample_rate,
	int audio_channel,
	int audio_recv_codec,
	int audio_send_codec,
	int video_recv_codec,
	int video_w_crop,
	int video_h_crop
){    
    //F_LOG;	
	int ret = 0;
     
    if(SID < 0) return -1;

	GET_LOCK( &g_CallbackContextLock );
	int status = connectionStatus;
	PUT_LOCK( &g_CallbackContextLock );

	if(status != PPPP_STATUS_ON_LINE){
		Log3("StartMediaStreams:[device not online.]");
		return -1;
	}

	if(TRY_LOCK(&PlayingLock) != 0){
		Log3("StartMediaStreams:[media stream already start.]");
		return -1;
	}

	if(TRY_LOCK(&DestoryLock) != 0){
		Log3("StartMediaStreams:[media stream closing.]");
		PUT_LOCK(&PlayingLock);
		return -1;
	}

	Log3("media stream start here.");

	// pppp://usr:pwd@livingstream:[channel id]
	// pppp://usr:pwd@replay/mnt/sdcard/replay/file
	memset(szURL,0,sizeof(szURL));

	AudioSampleRate = audio_sample_rate;
	AudioChannel = audio_channel;

	// only support channel mono, 16bit, 8KHz or 16KHz
	//   2 is come from 16bits/8bits = 2bytes
	// 100 is come from 10ms/1000ms
	Audio10msLength = audio_sample_rate * audio_channel * 2  / 100;
	AudioRecvFormat = audio_recv_codec;
	AudioSendFormat = audio_send_codec;
	VideoRecvFormat = video_recv_codec;

	MHCropSize = video_h_crop;
	MWCropSize = video_w_crop;

	Log3(
		"audio format info:[\n"
		"samplerate = %d\n"
		"channel = %d\n"
		"length in 10 ms is %d\n"
		"]\n",
		AudioSampleRate,AudioChannel,Audio10msLength);

	if(url != NULL){
		memcpy(szURL,url,strlen(url));
	}

	hAudioGetList->Clear();
	hAudioPutList->Clear();

	Log3("[OPENXL] INIT RESOURCE GET:[%d] PUT:[%d].",
		hAudioGetList->Used(),
		hAudioPutList->Used()
		);

    ret = LiveplayStart();
    
    if(ret == 0){
        videoPlaying = 1;
        audioPlaying = 1;
    }
        
	PUT_LOCK(&DestoryLock);

    LogX("media stream start %s.",ret == 0 ? "done" : "fail");

    return ret;
}

int CPPPPChannel::IOCmdSend(int type,char * msg,int len,int raw)
{
    while(1){
        int ret = -1;
        if(raw){
            ret = avSendIOCtrl(avIdx,type,msg,len);
        }else{
            ret = SendCmds(avIdx,type,msg,len,this);
        }
        
        Log3("[DEV:%s]=====>send IOCTRL cmd:[0x%04x].",szDID,type);
        
        if(ret == 0){
            return ret;
        }
    
        Log3("[DEV:%s]=====>send IOCTRL cmd failed with error:[%d].",szDID,ret);
    
        if(ret == AV_ER_SENDIOCTRL_ALREADY_CALLED){
            usleep(1000);
            continue;
        }else{
            return ret;
        }
    }
    
    return 0;
}

void CPPPPChannel::AlarmNotifyDoorBell(JNIEnv* hEnv,char *did, char *type, char *time )
{

	if( g_CallBack_Handle != NULL && g_CallBack_AlarmNotifyDoorBell != NULL )
	{
		jstring jdid	   = hEnv->NewStringUTF( szDID );
		jstring resultDid  = hEnv->NewStringUTF( did );
		jstring resultType = hEnv->NewStringUTF( type );
		jstring resultTime = hEnv->NewStringUTF( time );

		Log3("device msg push to %s with type:[%s] time:[%s].",did,type,time);

		hEnv->CallVoidMethod( g_CallBack_Handle, g_CallBack_AlarmNotifyDoorBell, jdid, resultDid, resultType, resultTime );

		hEnv->DeleteLocalRef( jdid );
		hEnv->DeleteLocalRef( resultDid  );
		hEnv->DeleteLocalRef( resultType );
		hEnv->DeleteLocalRef( resultTime );
	}
}

/*

*/

int CPPPPChannel::RecorderStart(
	int 		W,			// \BF\ED\B6\C8
	int 		H,			// \B8脽露\C8
	int 		FPS,		// 脰隆\C2\CA
	char *		SavePath	// 
){
	if(W == 0 || H == 0){
		W = this->MW;
		H = this->MH;
	}

	int Err = 0;

	if(FPS == 0){
		FPS = this->FPS;
    }else{
        this->FPS = FPS;
    }

	GET_LOCK(&CaptureLock);

	if(StartRecording(SavePath,FPS,W,H,this->AudioSampleRate,&AudioSaveLength) < 0){
		Log3("start recording with muxing failed.\n");
		goto jumperr;
	}

	recordingExit = 1;

	Err = pthread_create(
		&recordingThread,
		NULL,
		RecordingProcess,
		this);

	if(Err != 0){
		Log3("create av recording process failed.");
		CloseRecording();
		goto jumperr;
	}

	Log3("start recording process done.");

	PUT_LOCK(&CaptureLock);

	return  0;
	
jumperr:
	PUT_LOCK(&CaptureLock);

	return -1;
}

int CPPPPChannel::RecorderWrite(
	const char * 	FrameData,
	int				FrameSize,
	int				FrameCode, 	// audio or video codec [0|1]
	int				FrameType,	// keyframe or not [0|1]
	long long		Timestamp
){
//	Log3("frame write code and size:[%d][%d].\n",FrameCode,FrameSize);
	return WriteRecordingFrames((void*)FrameData,FrameSize,FrameCode,FrameType,Timestamp);
}

int CPPPPChannel::RecorderClose(){

	GET_LOCK(&CaptureLock);
	
	Log3("wait avi record process exit.");
	recordingExit = 0;
	if((long)recordingThread != -1){
		pthread_join(recordingThread,NULL);
		recordingThread = (pthread_t)-1;
	}
	Log3("avi record process exit done.");

	CloseRecording();

	PUT_LOCK(&CaptureLock);

	return 0;
}



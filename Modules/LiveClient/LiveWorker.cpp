#include "stdafx.h"
#include "uv.h"
#include "LiveWorker.h"
#include "LiveClient.h"
#include "liveObj.h"
#include "LiveIpc.h"

namespace LiveClient
{
	extern uv_loop_t *g_uv_loop;

    static map<string,CLiveWorker*>  m_workerMap;
    static CriticalSection           m_cs;

    string g_strRtpIP;            //< RTP����IP
    int    g_nRtpBeginPort;       //< RTP��������ʼ�˿ڣ�������ż��
    int    g_nRtpPortNum;         //< RTPʹ�õĸ�������strRTPPort��ʼÿ�μ�2����strRTPNum��
    int    g_nRtpCatchPacketNum;  //< rtp����İ�������
	int    g_nRtpStreamType;      //< rtp�������ͣ�����libLive��ps h264

    vector<int>     m_vecRtpPort;     //< RTP���ö˿ڣ�ʹ��ʱ����ȡ����ʹ�ý������·���
    CriticalSection m_csRTP;          //< RTP�˿���



    static int GetRtpPort()
    {
        MutexLock lock(&m_csRTP);

        int nRet = -1;
        auto it = m_vecRtpPort.begin();
        if (it != m_vecRtpPort.end()) {
            nRet = *it;
            m_vecRtpPort.erase(it);
        }

        return nRet;
    }

    static void GiveBackRtpPort(int nPort)
    {
        MutexLock lock(&m_csRTP);
        m_vecRtpPort.push_back(nPort);
    }


    /** ��ʱ���ٶ�ʱ����loop���Ƴ���� */
    static void stop_timer_close_cb(uv_handle_t* handle) {
        CLiveWorker* live = (CLiveWorker*)handle->data;
        if (live->m_bStop){
            live->Clear2Stop();
        } else {
            Log::debug("new client comed, and will not close live stream");
        }
    }

    /** �ͻ���ȫ���Ͽ�����ʱ�Ͽ�Դ�Ķ�ʱ�� */
	static void stop_timer_cb(uv_timer_t* handle) {
		CLiveWorker* live = (CLiveWorker*)handle->data;
		int ret = uv_timer_stop(handle);
		if(ret < 0) {
			Log::error("timer stop error:%s",uv_strerror(ret));
        }
        live->m_bStop = true;
        uv_close((uv_handle_t*)handle, stop_timer_close_cb);
	}

    /** CLiveWorker������ɾ��m_pLive�ȽϺ�ʱ��������event loop�����ʹ���̡߳� */
    static void live_worker_destory_thread(void* arg) {
        CLiveWorker* live = (CLiveWorker*)arg;
        SAFE_DELETE(live);
    }

    //////////////////////////////////////////////////////////////////////////

    CLiveWorker* CreatLiveWorker(string strCode)
    {
        Log::debug("CreatFlvBuffer begin");
        int rtpPort = GetRtpPort();
        if(rtpPort < 0) {
            Log::error("play failed %s, no rtp port",strCode.c_str());
            return nullptr;
        }

        CLiveWorker* pNew = new CLiveWorker(strCode, rtpPort);

        //if(!SipInstance::RealPlay(strCode, m_strRtpIP,  rtpPort))
        if(LiveIpc::RealPlay(strCode, g_strRtpIP,  rtpPort))
        {
            uv_thread_t tid;
            uv_thread_create(&tid, live_worker_destory_thread, pNew);
            Log::error("play failed %s",strCode.c_str());
            return nullptr;
        }
        Log::debug("RealPlay ok: %s",strCode.c_str());

        MutexLock lock(&m_cs);
        m_workerMap.insert(make_pair(strCode, pNew));

        return pNew;
    }

    CLiveWorker* GetLiveWorker(string strCode)
    {
        //Log::debug("GetWorker begin");

        MutexLock lock(&m_cs);
        auto itFind = m_workerMap.find(strCode);
        if (itFind != m_workerMap.end())
        {
            // �Ѿ�����
            return itFind->second;
        }

        //Log::error("GetWorker failed: %s",strCode.c_str());
        return nullptr;
    }

    bool DelLiveWorker(string strCode)
    {
        MutexLock lock(&m_cs);
        auto itFind = m_workerMap.find(strCode);
        if (itFind != m_workerMap.end())
        {
            // CLiveWorker������ɾ��m_pLive�ȽϺ�ʱ��������event loop�����ʹ���߳����ٶ���
            uv_thread_t tid;
            uv_thread_create(&tid, live_worker_destory_thread, itFind->second);

            m_workerMap.erase(itFind);
            return true;
        }

        Log::error("dosn't find worker object");
        return false;
    }

    //////////////////////////////////////////////////////////////////////////

    CLiveWorker::CLiveWorker(string strCode, int rtpPort)
        : m_strCode(strCode)
        , m_nPort(rtpPort)
        , m_nType(0)
        , m_pLive(nullptr)
        , m_bStop(false)
        , m_bOver(false)
        , m_bFlv(false)
        , m_bMp4(false)
        , m_bH264(false)
        , m_bTs(false)
        , m_bRtp(false)
    {
        m_pLive = new CLiveObj(rtpPort, this);
        m_pLive->StartListen();
    }

    CLiveWorker::~CLiveWorker()
    {
        //if(m_nType == 0) {
        //    if (!SipInstance::StopPlay(m_strCode)) {
        //        Log::error("stop play failed");
        //    }
        //} else {
        //    if (!SipInstance::StopRecordPlay(m_strCode, "")) {
        //        Log::error("stop play failed");
        //    }
        //}
        if(LiveIpc::StopPlay(m_strCode)) {
            Log::error("stop play failed");
        }
        SAFE_DELETE(m_pLive);
        GiveBackRtpPort(m_nPort);
        Log::debug("CLiveWorker release");
    }

    bool CLiveWorker::AddHandle(ILiveHandle* h, HandleType t)
    {
        if(t == HandleType::flv_handle) {
            m_bFlv = true;
            MutexLock lock(&m_csFlv);
            m_vecLiveFlv.push_back(h);
        } else if(t == HandleType::fmp4_handle) {
            m_bMp4 = true;
            MutexLock lock(&m_csMp4);
            m_vecLiveMp4.push_back(h);
        } else if(t == HandleType::h264_handle) {
            m_bH264 = true;
            MutexLock lock(&m_csH264);
            m_vecLiveH264.push_back(h);
        } else if(t == HandleType::ts_handle) {
            m_bTs = true;
            MutexLock lock(&m_csTs);
            m_vecLiveTs.push_back(h);
        } else if(t == HandleType::rtp_handle) {
            m_bRtp = true;
            MutexLock lock(&m_csRtp);
            m_vecLiveRtp.push_back((ILiveHandleRtp*)h);
        }

        //����ոտ����˽�����ʱ������Ҫ����ر�
        if(uv_is_active((const uv_handle_t*)&m_uvTimerStop)) {
            uv_timer_stop(&m_uvTimerStop);
            uv_close((uv_handle_t*)&m_uvTimerStop, stop_timer_close_cb);
        }
        return true;
    }

    bool CLiveWorker::RemoveHandle(ILiveHandle* h)
    {
        bool bFind = false;
        do {
            for(auto it = m_vecLiveFlv.begin(); it != m_vecLiveFlv.end(); it++) {
                if(*it == h) {
                    m_vecLiveFlv.erase(it);
                    bFind = true;
                    break;
                }
            }
            if(bFind) {
                if(m_vecLiveFlv.empty())
                    m_bFlv = true;
                break;
            }

            for(auto it = m_vecLiveMp4.begin(); it != m_vecLiveMp4.end(); it++) {
                if(*it == h) {
                    m_vecLiveMp4.erase(it);
                    bFind = true;
                    break;
                }
            }
            if(bFind) {
                if(m_vecLiveMp4.empty())
                    m_bMp4 = true;
                break;
            }

            for(auto it = m_vecLiveH264.begin(); it != m_vecLiveH264.end(); it++) {
                if(*it == h) {
                    m_vecLiveH264.erase(it);
                    bFind = true;
                    break;
                }
            }
            if(bFind) {
                if(m_vecLiveH264.empty())
                    m_bH264 = true;
                break;
            }

            for(auto it = m_vecLiveTs.begin(); it != m_vecLiveTs.end(); it++) {
                if(*it == h) {
                    m_vecLiveTs.erase(it);
                    bFind = true;
                    break;
                }
            }
            if(bFind) {
                if(m_vecLiveTs.empty())
                    m_bTs = true;
                break;
            }

            for(auto it = m_vecLiveRtp.begin(); it != m_vecLiveRtp.end(); it++) {
                if(*it == h) {
                    m_vecLiveRtp.erase(it);
                    bFind = true;
                    break;
                }
            }
            if(bFind) {
                if(m_vecLiveRtp.empty())
                    m_bRtp = true;
                break;
            }
        }while(0);

        if(m_vecLiveFlv.empty() && m_vecLiveMp4.empty() && m_vecLiveH264.empty()
            && m_vecLiveTs.empty() && m_vecLiveRtp.empty()) {
            if(m_bOver) {
                // ��ƵԴû�����ݣ���ʱ������Ҫ��ʱ
                Clear2Stop();
            } else {
                // ��ƵԴ��Ȼ���ӣ���ʱ20�������ٶ����Ա��ʱ�������������ܿ��ٲ���
                uv_timer_init(g_uv_loop, &m_uvTimerStop);
                m_uvTimerStop.data = this;
                uv_timer_start(&m_uvTimerStop, stop_timer_cb, 20000, 0);
            }
        }
        return true;
    }

	void CLiveWorker::Clear2Stop() {
        if(m_vecLiveFlv.empty() && m_vecLiveMp4.empty() && m_vecLiveH264.empty()
            && m_vecLiveTs.empty() && m_vecLiveRtp.empty()) {
			Log::debug("need close live stream");
            //���ȴ�map�����߶���
            DelLiveWorker(m_strCode);
		}
	}

    void CLiveWorker::push_flv_stream(int eType, char* pBuff, int nLen)
    {
        MutexLock lock(&m_csFlv);
        for (auto h : m_vecLiveFlv)
        {
            h->push_video_stream(pBuff, nLen);
        }   
    }

    void CLiveWorker::push_h264_stream(char* pBuff, int nLen)
    {
        MutexLock lock(&m_csH264);
        for (auto h : m_vecLiveH264)
        {
            h->push_video_stream(pBuff, nLen);
        } 
    }

    void CLiveWorker::push_ts_stream(char* pBuff, int nLen)
    {
        MutexLock lock(&m_csTs);
        for (auto h : m_vecLiveTs)
        {
            h->push_video_stream(pBuff, nLen);
        } 
    }

    void CLiveWorker::push_fmp4_stream(int eType, char* pBuff, int nLen)
    {
        MutexLock lock(&m_csMp4);
        for (auto h : m_vecLiveMp4)
        {
            h->push_video_stream(pBuff, nLen);
        } 
    }

    void CLiveWorker::push_rtp_stream(char* pBuff, int nLen)
    {
        MutexLock lock(&m_csRtp);
        for (auto h : m_vecLiveRtp)
        {
            h->push_video_stream(pBuff, nLen);
        } 
    }

    void CLiveWorker::push_rtcp_stream(char* pBuff, int nLen)
    {
        MutexLock lock(&m_csRtp);
        for (auto h : m_vecLiveRtp)
        {
            h->push_rtcp_stream(pBuff, nLen);
        } 
    }

    void CLiveWorker::stop()
    {
        //��ƵԴû�����ݲ���ʱ
        Log::debug("no data recived any more, stopped");
        //״̬�ı�Ϊ��ʱ����ʱǰ��ȫ���Ͽ�������Ҫ��ʱ��ֱ������
        m_bOver = true;

        for (auto worker : m_vecLiveFlv) {
            worker->stop();
        }
        for (auto worker : m_vecLiveMp4) {
            worker->stop();
        }
        for (auto worker : m_vecLiveH264) {
            worker->stop();
        }
        for (auto worker : m_vecLiveTs) {
            worker->stop();
        }
        for (auto worker : m_vecLiveRtp) {
            worker->stop();
        }
    }

    string CLiveWorker::GetClientInfo()
    {
        string strResJson = "{\"root\":[";
        MutexLock lock(&m_cs);
        for (auto w : m_workerMap)
        {
            CLiveWorker *worker = w.second;
            for(auto h : worker->m_vecLiveFlv){
                strResJson += h->get_clients_info();
            }
        }

        strResJson = StringHandle::StringTrimRight(strResJson,',');
        strResJson += "]}";
        return strResJson;
    }

}
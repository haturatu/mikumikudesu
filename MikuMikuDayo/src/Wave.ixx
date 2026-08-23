/*
 オーディオファイルを16bitステレオのPCMデータとして読み込むためのモジュール
 ハイレゾ音源？なんですかそれ

 動画にも対応！

 使う前にCoInitializeEx()でComの初期化を忘れずに
 
 MMXXIV,MMXXV,MMXXVI (c) SANDMAN presents!!


【つかいかた】
 Wave.ixxをプロジェクトのソースファイルに加えて、import Wave; だけでOK
 VisualStudioで「DirectXゲーム開発」がインストールされていれば追加のライブラリもたぶん不要

【はじめかた・音声編】
 1. COMの初期化をする(CoInitializeEx)
 2. Wave::Init()を呼ぶ
 3. Wave::Data data("wavefile.wav"); などとして波形データの読み込み。wav、m4a、mp3等に対応。FFMpeg入れてればoggも行けるらしい
 4. Wave::Player player; player.Set(data); などとしてプレイヤーオブジェクトにセット
 5. player.Play(); で再生開始(非同期)
 6. 再生終了まで待ちたい時は while(player.Playing()); などする
 7. Wave::Cleanup(); でお片付け(data, playerの寿命が切れてからやった方が安全と思う)

【はじめかた・動画編】
1.音声編の2.の手順までと同じで初期化
2.Movie movie などとしてインスタンス作成
3.movie.Load("mov.mp4")などとしてmov.mp4を読み込む
4.movie.GetFrame(time, fetched) として再生開始からtime秒後のフレームを取得。fetchedはbool型変数で、取得結果が前のフレームと同じ内容である場合はfalseが返る
5.movie.frameBufferにアクセスして取得したフレームの内容を得る。内容は無圧縮だがフォーマットはFourCC()に従った形式になっているので注意(この内容をGPUに転送してGPUで変換する事を前提にしている)


【注意】
 Player::Set(data)でセットされた波形データオブジェクトは、playerに他のDataオブジェクトがSetされる、またはplayerの寿命が来るまでは破棄しないでね
 
【波形データを直接読み書きしたい時は】
 ・Data::PCM[]には無圧縮・16bit・ステレオに展開された波形データがint16_t型で入ってます
 ・PCM[0]は最初のサンプルのLチャンネル、PCM[1]は最初のサンプルのRチャンネル分が入ります
 ・サンプリング周波数はData::Freq()で取れます
 ・ステレオなので再生時間1秒あたりPCM[].size() == Freq()*2になります
 ・元データが8bitモノラルでも16bitステレオに展開されます。この場合はLチャンネルとRチャンネルに同じデータが入ります
 ・24bit音源など16bitより分解能の高い音源でも16bitに縮められます


【更新履歴】
2026-0309 Movie::Load()で各API呼び出し時にThrowIfFailedを何か所か付け忘れていたのを追加した
2026-0127 Data::Duration()より後から再生しようとするとエラーになっていたのでチェックして再生しないようにした(エラーや例外などは起こさない)

*/



module;

#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

//Windows
#include <Windows.h>
#include <wrl/client.h>

//WMF
#include <initguid.h> // GUID比較用
#include <mfapi.h>
#include <Mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mfmediaengine.h>
#include <propvarutil.h>
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

//XAudio2
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")

//DirectX12
#include <d3dx12.h>
#include <d3d12video.h>
#pragma comment(lib,"dxguid.lib")

export module Wave;

import YRZ;

export namespace Wave {
    using namespace Microsoft::WRL;
    namespace fs = std::filesystem;

    template<typename T>
    struct VoiceDeleter {
        void operator()(T* p) const
        {
            if (p) p->DestroyVoice();
        }
    };

    ComPtr<IXAudio2> XAudio2;
    std::unique_ptr<IXAudio2MasteringVoice, VoiceDeleter<IXAudio2MasteringVoice>> MasteringVoice;

    //初期化。使い始める前にこれ呼んでね、CointializeExを先に呼んでね
    void Init();    
    //終了。使い終わったら呼んでね
    void Cleanup();

    //マスター音量の取得 0でミュート 1でMAX
    float Volume();
    //マスター音量の設定 0でミュート 1でMAX
    void Volume(float v);

    //16bitステレオ波形データ
	class Data {
    private:
        double m_duration = 0;
        std::uint32_t m_freq = 44100;
    public:
        std::vector<std::int16_t>PCM;  //再生可能な形式に変換されたデータ LRLR...の順
        //サンプリングレートfreq[Hz]で読み込み。0の時は元データに合わせる
        //読み込み後は元データの量子化数によらず範囲[-32768..32767]のint16_tに変換される
        void Load(const wchar_t* filename, std::uint32_t freq=0);
        //再生時間[秒]
        double Duration() const { return m_duration; }; 
        //格納しているデータの想定しているサンプリング周波数
        std::uint32_t Freq() const { return m_freq; }   
        //波形データのクリア
        void Clear() { *this = {}; }
        //コンストラクタ
        Data() {};
        Data(const wchar_t* filename, std::uint32_t freq = 0) { Load(filename, freq); }
    };

    //波形データ再生クラス。SetDataで波形データを指定してPlayで再生
    //再生するデータがSetされていない場合は何もしないだけで例外は起こさない
    class Player {
    private:
        std::uint64_t m_startPos = 0;   //再生開始位置
        Data* m_data = nullptr;
        std::unique_ptr<IXAudio2SourceVoice, VoiceDeleter<IXAudio2SourceVoice>> m_sourceVoice;
    public:
        //再生するデータの設定。再生中にdataが解放されてはならぬ
        void Set(Data& data);
        //再生するデータが無い状態にする。dataは解放できるようになる
        void Reset() { Stop(); m_sourceVoice = nullptr; m_data = nullptr; };
        //再生。再生位置startは秒単位、0以上でないとダメです
        void Play(double start=0);
        //停止
        void Stop();
        //現在の再生位置を秒単位で返す
        double PositionInSecond();
        //再生中のサンプル番号(PCM内での位置)を返す
        uint64_t SamplePosition(); 
        //再生中ならtrue
        bool Playing();
        //音量の設定 0でミュート 1でMAX
        void Volume(float vol);
        //音量の取得 0でミュート 1でMAX
        float Volume();
        //再生速度 1.0で通常の速度(周波数も変わる)
        void Speed(float ratio);
        float Speed();
        //コンストラクタ
        Player() {};
        Player(Data& data) { Set(data); };
        //使用中のデータへの参照。nullptrの時は何もセットされてない
        Data* LoadedData() { return m_data; }
        //使用中のIXAudio2SourceVoiceインターフェイス
        IXAudio2SourceVoice* SourceVoice() { if (m_sourceVoice) return m_sourceVoice.get(); else return nullptr; }
    };

    //動画の1フレームをゲットするクラス
    class Movie {
    private:
        fs::path m_path;
        double m_duration = 0;
        LONGLONG m_prevT = -1; //前回GetFrameした位置[100ns] マイナスの場合は一度もGetFrameされてない
        double m_fps = 0;
        std::string m_fourCC;   //デコード結果のfourCC(YUY2, NV12, RGB32のどれか)
        ComPtr<IMFSourceReader> m_reader;
        uint32_t m_width=0, m_height = 0, m_bpp = 0, m_frameBytes = 0;
    public:
        std::vector<uint8_t> frameBuffer;    //デコード結果保存用、fourCCに従った無圧縮形式になっている
        void Load(const fs::path& filename);
        void GetFrame(double time, bool& fetched); //前回GetFrameしたフレーム内容と同じだった場合、fetchedにはfalseが帰る
        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }
        std::string FourCC() const { return m_fourCC; }
        int BitsPerPixel() const { return m_bpp; }
    };

    //動画の1フレームゲットクラス(GPUデコード版)
    class GPUMovie {
    private:
        YRZ::DXR* m_dxr;
        ComPtr<ID3D12VideoDevice> m_videoDevice;
        ComPtr<ID3D12VideoDecoder> m_decoder;
        ComPtr<ID3D12VideoDecoderHeap> m_heap;
        ComPtr<IMFSourceReader> m_reader;
        ComPtr<ID3D12CommandQueue> m_cmdQ;
        ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
        ComPtr<ID3D12VideoDecodeCommandList> m_cmdList;
        UINT m_width = 0, m_height = 0;
        size_t m_frameSize = 0;
        GUID m_subtype;
        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
        bool m_useD3D12 = false;
        double m_duration = 0;
        LONGLONG m_prevT = -1; //前回GetFrameした位置[100ns] マイナスの場合は一度もGetFrameされてない
        double m_fps = 0;
        YRZ::Buf m_upload;
    public:
        YRZ::Res Output;
        bool Load(YRZ::DXR* dxr, const fs::path& filename, bool& fallbackNeeded);
        bool GetFrame(double time);
    };
};

module : private;




namespace Wave {

	//例外(このモジュールからのみ発出される)
	class WaveException : public std::runtime_error
	{
	private:
		std::string message;
	public:
		WaveException(const std::string& msg) : runtime_error(msg) {};
	};

	HRESULT ThrowIfFailed(HRESULT hr, const std::string& msg)
	{
		if (FAILED(hr)) {
			throw WaveException(std::format("{}\nhresult:{:x}", msg, hr));
		}
		return hr;
	}

	void Data::Load(const wchar_t* filename, std::uint32_t freq)
	{
        ComPtr<IMFSourceReader> pReader;
        ThrowIfFailed(MFCreateSourceReaderFromURL(filename, nullptr, &pReader), "cant' open audio file {}");

        DWORD streamIndex, flags;
        LONGLONG llTimeStamp;
        ComPtr<IMFSample> pSample;

        //入力メディアタイプから情報を得る(現在の実装ではサンプリング周波数しか使ってない)
        ComPtr<IMFMediaType> pMediaType;
        std::uint32_t sampleRate;
        std::uint32_t numChannels;
        std::uint32_t bitsPerSample;
        ThrowIfFailed(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pMediaType), "GetCurrentMediaType failed");
        pMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
        pMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
        pMediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
        m_freq = freq ? freq : sampleRate;

        // 出力メディアタイプの設定
        ComPtr<IMFMediaType> pAudioType;
        ThrowIfFailed(MFCreateMediaType(&pAudioType), "MFCreateMediaType failed.");

        //無圧縮PCM 16bit ステレオ freq[Hz]として出力する
        pAudioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pAudioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pAudioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pAudioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_freq);
        pAudioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ThrowIfFailed(pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pAudioType.Get()), "SetCurrentMediaType failed.");

        UINT32 bytesPer1ch = 2;        //1chで何バイトか?
        UINT32 bytesPerAll = 4;        //全chで何バイトか?

        PCM.clear();
        PCM.shrink_to_fit();    //clearするだけだとcapacity分のメモリは確保されっぱなしなのでこれは必要
        m_duration = 0;

        // オーディオサンプルの取得
        /*
        PROPVARIANT var;
        PropVariantInit(&var);
        InitPropVariantFromInt64(0, &var);
        pReader->SetCurrentPosition(GUID_NULL, var);
        */
        int iter = 0;
        while (SUCCEEDED(pReader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &llTimeStamp, pSample.ReleaseAndGetAddressOf()))) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;
            if (pSample) {
                ComPtr<IMFMediaBuffer> pBuffer;
                pSample->ConvertToContiguousBuffer(&pBuffer);
                BYTE* pAudioData = nullptr;
                DWORD cbBuffer = 0;
                pBuffer->Lock(&pAudioData, nullptr, &cbBuffer);
                DWORD nSamples = cbBuffer / 2;
                    
                //PCMデータを追加
                size_t ps = PCM.size();
                PCM.resize(ps + nSamples);
                memcpy(PCM.data() + ps, pAudioData, cbBuffer);
                    
                pBuffer->Unlock();
            }
            //std::cout << llTimeStamp << std::endl;
        }

        m_duration = (double)PCM.size() / 2 / m_freq;  //ステレオ m_freq[Hz]

    }

    void Init() {
        ThrowIfFailed(MFStartup(MF_VERSION), "MFStartup failed");
        ThrowIfFailed(XAudio2Create(XAudio2.ReleaseAndGetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR), "XAudio2Create failed.");
        IXAudio2MasteringVoice* rawMasteringVoice;
        ThrowIfFailed(XAudio2->CreateMasteringVoice(&rawMasteringVoice), "CreateMasteringVoice failed.");
        MasteringVoice.reset(rawMasteringVoice);
    }

    void Cleanup() {
        MasteringVoice = nullptr;
        XAudio2 = nullptr;
        MFShutdown();
    }

    float Volume() {
        if (MasteringVoice) {
            float r;
            MasteringVoice->GetVolume(&r);
            return r;
        } else {
            return 1;
        }
    }

    void Volume(float v) {
        if (MasteringVoice) {
            MasteringVoice->SetVolume(v);
        }
    }

    void Player::Set(Data& data)
    {
        Reset();

        m_data = &data;
        WAVEFORMATEX waveFormat = { 0 };
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = 2; // ステレオ
        waveFormat.nSamplesPerSec = data.Freq(); // サンプルレート
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
        waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

        IXAudio2SourceVoice* namapo;
        ThrowIfFailed(XAudio2->CreateSourceVoice(&namapo, &waveFormat), "CreateSourceVoice failed.");
        m_sourceVoice.reset(namapo);
    }

    void Player::Play(double start)
    {
        //2026-0127 Durationより後から再生しようとするとエラーになるのでチェックするようにした
        if (m_sourceVoice == nullptr || start >= m_data->Duration())
            return;

        if (Playing())
            Stop();

        XAUDIO2_BUFFER buffer = {};
        buffer.AudioBytes = static_cast<UINT32>(m_data->PCM.size() * 2);
        buffer.pAudioData = reinterpret_cast<BYTE*>(m_data->PCM.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.PlayBegin = UINT32(start * m_data->Freq());
        m_startPos = buffer.PlayBegin;
           
        //現在のSamplesPlayedを引く
        // SamplesPlayedは「オブジェクトが作られてから再生されたサンプル数」なのでPlayからの経過時間が累積されていく
        // このため、「現在の再生位置」を知りたい場合はPlayメソッド発行からのSamplesPlayedの差分が大事
        XAUDIO2_VOICE_STATE state;
        m_sourceVoice->GetState(&state);
        m_startPos -= state.SamplesPlayed;

        ThrowIfFailed(m_sourceVoice->SubmitSourceBuffer(&buffer), "SubmitSourceBuffer failed.");

        m_sourceVoice->Start(0);
    }

    void Player::Stop()
    {
        if (m_sourceVoice) {
            m_sourceVoice->Stop(0);
            //↓これやらないとPlay(100)→(しばらく待つ)→Stop→Play(100)とやっても100からではなく続きから再生になる
            m_sourceVoice->FlushSourceBuffers();    
        }
    }

    uint64_t Player::SamplePosition()
    {
        if (m_sourceVoice) {
            XAUDIO2_VOICE_STATE state;
            m_sourceVoice->GetState(&state);
            return (state.SamplesPlayed + m_startPos)*2;    //ステレオなので
        }
        return 0;
    }

    double Player::PositionInSecond()
    {
        return (double)SamplePosition() / 2 / m_data->Freq();
    }

    bool Player::Playing()
    {
        if (m_sourceVoice) {
            XAUDIO2_VOICE_STATE state;
            m_sourceVoice->GetState(&state);
            return state.BuffersQueued != 0;
        }
        return false;
    }

    void Player::Volume(float vol)
    {
        if (m_sourceVoice)
            m_sourceVoice->SetVolume(vol);
    }
    
    float Player::Volume()
    {
        if (m_sourceVoice) {
            float r;
            m_sourceVoice->GetVolume(&r);
            return r;
        }
        return 1;
    }

    void Player::Speed(float ratio)
    {
        if (m_sourceVoice) {
            m_sourceVoice->SetFrequencyRatio(ratio);
        }
    }

    float Player::Speed()
    {
        if (m_sourceVoice) {
            float r;
            m_sourceVoice->GetFrequencyRatio(&r);
            return r;
        }
        return 1;
    }


    void Movie::Load(const fs::path& filename)
    {
        m_prevT = -1;
        m_path = filename;
        ThrowIfFailed(MFCreateSourceReaderFromURL(filename.wstring().c_str(), NULL, m_reader.ReleaseAndGetAddressOf()), "Movie::Load() : MFCreateSourceReaderFromURL failed");
        
        std::vector<GUID> preferredFormats = { MFVideoFormat_ARGB32, MFVideoFormat_YUY2, MFVideoFormat_NV12 };
        /*MFVideoFormat_I420, MFVideoFormat_RGB24, MFVideoFormat_RGB555, MFVideoFormat_RGB565, MFVideoFormat_RGB32, MFVideoFormat_UYVY,
        MFVideoFormat_NV11, MFVideoFormat_NV21, MFVideoFormat_P010, MFVideoFormat_P016, MFVideoFormat_P210, MFVideoFormat_P216};*/
        std::vector<std::string> candyCC = { "ARGB", "YUY2", "NV12" };    //ARGBはてきとう。あくまでこのモジュールでの識別用
        std::vector<uint32_t> candyBpp = { 32,16,12 };

        //デコードの設定
        ComPtr<IMFMediaType> pType;
        ThrowIfFailed(MFCreateMediaType(pType.GetAddressOf()), "MFCreateMediaType failed");
        ThrowIfFailed(pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "pType->SetGUID failed");


        // メディアタイプからピクセルフォーマットを取得
        ThrowIfFailed(m_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType), "Movie::Load() : m_reader->GetCurrentMediaType failed");
        GUID subtype = { 0 };
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);


        bool ok = false;
        HRESULT hr;
        for (int i = 0; auto & f:preferredFormats) {
            pType->SetGUID(MF_MT_SUBTYPE, f);
            hr = m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType.Get());
            if (SUCCEEDED(hr)) {
                m_fourCC = candyCC[i];
                m_bpp = candyBpp[i];
                ok = true;
                break;
            }
            i++;
        }
        if (!ok) {
            ThrowIfFailed(hr, "Movie::Load() : m_reader->SetCurrentMediaType failed, (decoded pixel format must be YUY2 or NV12 or ARGB32)");
        }
        
        //サイズの取得
        ComPtr<IMFMediaType> pMediaType;
        ThrowIfFailed(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, pMediaType.GetAddressOf()), "Movie::Load() : m_reader->GetCurrentMediaType failed");
        ThrowIfFailed(MFGetAttributeSize(pMediaType.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height), "Movie::Load() : MFGetAttributeSize failed");

        //再生時間の取得
        LONGLONG duration = 0;
        PROPVARIANT var;
        ThrowIfFailed(m_reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var), "Movie::Load() : m_reader->GetPresentationAttribute failed");
        duration = var.uhVal.QuadPart;
        PropVariantClear(&var);
        m_duration = (double)duration / 1e+7;

        uint32_t numerator, denominator;
        ThrowIfFailed(MFGetAttributeRatio(pMediaType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator), "Movie::Load() : MFGetAttributeRatio failed");
        m_fps = (double)numerator / denominator;

        //フレームバッファの作成
        m_frameBytes = m_width * m_height * m_bpp / 8;
        frameBuffer.resize(m_frameBytes);
    }

    //timeと同時刻または直前のフレームを得る(0ms,33ms,66ms,100ms...と動画が続くとき、target=60msが指定された場合は33msのフレームを返す
    void Movie::GetFrame(double time, bool& fetched)
    {
        fetched = false;
        LONGLONG target = (LONGLONG)(min(m_duration, max(0, time)) * 1e+7); //はみ出てる範囲が指定された場合は納める
        LONGLONG frameT = (LONGLONG)(1e+7 / m_fps); //1フレーム分の時間
        LONGLONG lastT = (LONGLONG)(m_duration * 1e+7); //動画の終わり

        //前回GetFrameできた時刻と、指定された時刻の差がフレームレート未満だったら帰る
        if (m_prevT >= 0 && m_prevT <= target && target < (m_prevT + frameT)) {
            return;
        }

        //前回GetFrameした時刻から0.5秒以下だったらシークせず、ReadSampleで順送りする(GOPの頻度が分からないので)
        //逆再生の場合はシークする
        if (!(target > m_prevT && (target - m_prevT <= frameT*m_fps/2))) {
            PROPVARIANT var;
            InitPropVariantFromInt64(target, &var);
            ThrowIfFailed(m_reader->SetCurrentPosition(GUID_NULL, var), "m_reader->SetCurrentPosition failed");
            PropVariantClear(&var);
        }


        DWORD streamIndex, flags;
        LONGLONG timestamp;
        ComPtr<IMFSample> pSample;
        int cacheIndex = 0;
        //シークされた地点から順送り
        while (1) {
            ThrowIfFailed(m_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0, &streamIndex, &flags, &timestamp, pSample.ReleaseAndGetAddressOf()), "m_reader->ReadSample failed");
            //目標点か動画の最後まで来たら順送り終了
            if (timestamp+frameT >= target)
                break;
            if (timestamp >= lastT)
                break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;
        }

        if (pSample) {
            ComPtr<IMFMediaBuffer> pBuffer;
            ThrowIfFailed(pSample->ConvertToContiguousBuffer(pBuffer.ReleaseAndGetAddressOf()), "pSample->ConvertToContiguousBuffer failed");

            BYTE* pData = nullptr;
            DWORD maxLength = 0, currentLength = 0;
            auto hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
            if (SUCCEEDED(hr)) {
                fetched = true;
                memcpy_s(frameBuffer.data(), frameBuffer.size(), pData, min(currentLength, frameBuffer.size()));
                pBuffer->Unlock();
            }
            m_prevT = timestamp;
        }

    }

    /*************************************************************
    * GPU video decoder対応
    *************************************************************/

    struct StreamInfo {
        GUID subtype{};
        UINT width = 0, height = 0;
        UINT frNum = 0, frDen = 1;
        MFVideoInterlaceMode interlace = MFVideoInterlace_Unknown;
    };

    static UINT Lo32(UINT64 v) { return (UINT)(v & 0xFFFFFFFFu); }
    static UINT Hi32(UINT64 v) { return (UINT)(v >> 32); }

    bool GetStreamInfo(IMFSourceReader* reader, StreamInfo& out) {
        ComPtr<IMFMediaType> nativeType;
        ThrowIfFailed(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType), "reader->GetNativeMediaType failed");

        ThrowIfFailed(nativeType->GetGUID(MF_MT_SUBTYPE, &out.subtype), "nativeType->GetGUID failed");

        UINT64 fs = 0;
        if (SUCCEEDED(nativeType->GetUINT64(MF_MT_FRAME_SIZE, &fs))) {
            out.width = Lo32(fs);
            out.height = Hi32(fs);
        }
        UINT64 fr = 0;
        if (SUCCEEDED(nativeType->GetUINT64(MF_MT_FRAME_RATE, &fr))) {
            out.frNum = Lo32(fr);
            out.frDen = Hi32(fr);
            if (out.frDen == 0) out.frDen = 1;
        }
        UINT32 im = MFVideoInterlace_Unknown;
        nativeType->GetUINT32(MF_MT_INTERLACE_MODE, &im);
        out.interlace = (MFVideoInterlaceMode)im;
        return true;
    }

    // ========== 候補生成 ==========
    struct DecodeCandidates {
        std::vector<D3D12_VIDEO_DECODE_CONFIGURATION> configs;
        std::vector<DXGI_FORMAT> surfaces; // NV12/P010
    };

    static void PushProfile(DecodeCandidates& c, const GUID& profile) {
        D3D12_VIDEO_DECODE_CONFIGURATION cfg{};
        cfg.DecodeProfile = profile;
        cfg.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
        cfg.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;
        c.configs.push_back(cfg);
    }

    DecodeCandidates MakeCandidatesFromSubtype(const GUID& subtype) {
        DecodeCandidates c{};
        if (subtype == MFVideoFormat_H264) {
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_H264);
            c.surfaces = { DXGI_FORMAT_NV12 };
        } else if (subtype == MFVideoFormat_HEVC) {
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN);
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10);
            // 10-bit 優先
            c.surfaces = { DXGI_FORMAT_P010, DXGI_FORMAT_NV12 };
        } else if (subtype == MFVideoFormat_VP90) { // VP9
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_VP9);
            c.surfaces = { DXGI_FORMAT_P010, DXGI_FORMAT_NV12 };
        } else if (subtype == MFVideoFormat_AV1) {
            // AV1 は 8/10-bit 両対応候補を用意（非対応GPUでは後で落ちる）
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0);
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2);
            c.surfaces = { DXGI_FORMAT_P010, DXGI_FORMAT_NV12 };
        } else if (subtype == MFVideoFormat_WVC1 || subtype == MFVideoFormat_WMV3) {
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_VC1);
            c.surfaces = { DXGI_FORMAT_NV12 };
        } else if (subtype == MFVideoFormat_MPEG2) {
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_MPEG2);
            c.surfaces = { DXGI_FORMAT_NV12 };
        } else {
            // 未知→よく使われる構成を試す
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_H264);
            PushProfile(c, D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN);
            c.surfaces = { DXGI_FORMAT_NV12, DXGI_FORMAT_P010 };
        }
        return c;
    }

    // ========== 構成選択 ==========
    struct ChosenDecode {
        D3D12_VIDEO_DECODE_CONFIGURATION config{};
        DXGI_FORMAT surfaceFormat = DXGI_FORMAT_NV12;
        UINT width = 0, height = 0;
        bool supported = false;
        D3D12_VIDEO_DECODE_TIER tier = D3D12_VIDEO_DECODE_TIER_NOT_SUPPORTED;
    };

    bool PickDecoderConfig(
        ID3D12VideoDevice* videoDevice,
        const StreamInfo& info,
        const DecodeCandidates& cand,
        ChosenDecode& out)
    {
        out = {};
        out.width = info.width; out.height = info.height;

        // より良い候補（10-bit/P010、高い Tier）を優先するため、詰めて探索
        for (const auto& cfgIn : cand.configs) {
            for (DXGI_FORMAT fmt : cand.surfaces) {
                D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT qs{};
                qs.NodeIndex = 0;
                qs.Configuration = cfgIn;
                qs.Width = info.width;
                qs.Height = info.height;
                qs.DecodeFormat = fmt;
                qs.FrameRate.Numerator = info.frNum ? info.frNum : 60000;
                qs.FrameRate.Denominator = info.frDen ? info.frDen : 1000;
                qs.BitRate = 0;
                //qs.InterlaceType = ToCodedInterlace(info.interlace);

                if (SUCCEEDED(videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &qs, sizeof(qs)))) {
                    if (qs.SupportFlags & D3D12_VIDEO_DECODE_SUPPORT_FLAG_SUPPORTED) {
                        // ここで最初に通ったものを仮に採用し、Tier がより高い候補が見つかったら置き換える
                        if (!out.supported || qs.DecodeTier > out.tier) {
                            out.supported = true;
                            out.config = qs.Configuration; // SDK により SelectedConfig 名の場合あり。手元のヘッダに合わせてください。
                            //out.config.InterlaceType = qs.InterlaceType;
                            out.surfaceFormat = fmt;
                            out.tier = qs.DecodeTier;
                        }
                    }
                }
            }
        }
        return out.supported;
    }

    // ========== デコーダ/ヒープ生成 ==========
    bool CreateDecoderAndHeap(
        ID3D12VideoDevice* videoDevice,
        const ChosenDecode& chosen,
        UINT numDpbSlots,
        ComPtr<ID3D12VideoDecoder>& decoder,
        ComPtr<ID3D12VideoDecoderHeap>& heap)
    {
        D3D12_VIDEO_DECODER_DESC decDesc{};
        decDesc.Configuration = chosen.config;
        ThrowIfFailed(videoDevice->CreateVideoDecoder(&decDesc, IID_PPV_ARGS(&decoder)), "videoDevice->CreateVideoDecoder failed");

        D3D12_VIDEO_DECODER_HEAP_DESC heapDesc{};
        heapDesc.DecodeHeight = chosen.height;
        heapDesc.DecodeWidth = chosen.width;
        heapDesc.Format = chosen.surfaceFormat;
        heapDesc.Configuration = chosen.config;
        heapDesc.MaxDecodePictureBufferCount = numDpbSlots;
        ThrowIfFailed(videoDevice->CreateVideoDecoderHeap(&heapDesc, IID_PPV_ARGS(&heap)), "videoDevice->CreateVideoDecoderHeap failed");
        return true;
    }


    struct BitReader {
        const uint8_t* data{};
        size_t size{};
        size_t bitpos{};
        uint32_t readBits(size_t n) {
            uint32_t r = 0;
            for (size_t i = 0; i < n; i++) {
                r <<= 1;
                if (bitpos / 8 < size) {
                    r |= (data[bitpos / 8] >> (7 - (bitpos % 8))) & 1;
                }
                bitpos++;
            }
            return r;
        }
        uint32_t readUE() {
            size_t zeros = 0;
            while (bitpos < size * 8 && readBits(1) == 0) zeros++;
            if (zeros > 31) return 0; // guard
            uint32_t suffix = zeros ? readBits(zeros) : 0;
            return ((1u << zeros) - 1) + suffix;
        }
        int32_t readSE() {
            uint32_t ue = readUE();
            return (ue & 1) ? int32_t((ue + 1) / 2) : -int32_t(ue / 2);
        }
    };

    // Emulation prevention bytes(0x000003) を除去してRBSPへ
    std::vector<uint8_t> ToRBSP(const uint8_t* nal, size_t len) {
        std::vector<uint8_t> rbsp;
        rbsp.reserve(len);
        int zeroCount = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t b = nal[i];
            if (zeroCount == 2 && b == 0x03) { // skip EPB
                zeroCount = 0;
                continue;
            }
            rbsp.push_back(b);
            if (b == 0x00) zeroCount++; else zeroCount = 0;
        }
        return rbsp;
    }

    // Annex Bのstart code区切りでNALUを抽出
    std::vector<std::vector<uint8_t>> SplitAnnexB(const uint8_t* data, size_t size) {
        std::vector<std::vector<uint8_t>> nalus;
        size_t i = 0;
        auto isStart = [&](size_t p)->size_t {
            if (p + 3 < size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) return 3;
            if (p + 4 < size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) return 4;
            return 0;
            };
        while (i < size) {
            size_t sc = isStart(i);
            if (!sc) { i++; continue; }
            size_t start = i + sc;
            i = start;
            while (i < size) {
                size_t sc2 = isStart(i);
                if (sc2) break;
                i++;
            }
            nalus.emplace_back(data + start, data + i);
        }
        return nalus;
    }

    // AVCC(h264)/HVCC(h265) のConfigRecordから NALユニット群を取り出す
    // 戻り値はNAL単位のベクタ（SPS等）
    std::vector<std::vector<uint8_t>> ParseAVCDecoderConfigurationRecord(const uint8_t* blob, size_t size) {
        // ISO/IEC 14496-15 AVCDecoderConfigurationRecord
        if (size < 7) return {};
        const uint8_t* p = blob;
        uint8_t configurationVersion = p[0];
        (void)configurationVersion;
        uint8_t nalLengthSize = (p[4] & 0x3) + 1; // usually 4
        uint8_t numOfSPS = p[5] & 0x1F;
        p += 6;
        size_t remain = size - 6;
        std::vector<std::vector<uint8_t>> out;

        auto get16 = [&](const uint8_t* q) { return (size_t(q[0]) << 8) | size_t(q[1]); };

        for (uint8_t i = 0; i < numOfSPS; ++i) {
            if (remain < 2) return out;
            size_t n = get16(p); p += 2; remain -= 2;
            if (remain < n) return out;
            out.emplace_back(p, p + n);
            p += n; remain -= n;
        }
        if (remain < 1) return out;
        uint8_t numOfPPS = *p++; remain--;
        for (uint8_t i = 0; i < numOfPPS; ++i) {
            if (remain < 2) return out;
            size_t n = get16(p); p += 2; remain -= 2;
            if (remain < n) return out;
            out.emplace_back(p, p + n);
            p += n; remain -= n;
        }
        (void)nalLengthSize; // デマルク時に使用。ここでは構成抽出だけ。
        return out;
    }

    std::vector<std::vector<uint8_t>> ParseHEVCDecoderConfigurationRecord(const uint8_t* blob, size_t size) {
        // hvcC (HEVCDecoderConfigurationRecord) 簡易抽出
        if (size < 23) return {};
        const uint8_t* p = blob;
        p += 21; // general_xxx までスキップ
        uint8_t numOfArrays = *p++;
        size_t remain = size - (p - blob);
        std::vector<std::vector<uint8_t>> out;

        auto get16 = [&](const uint8_t* q) { return (size_t(q[0]) << 8) | size_t(q[1]); };

        for (uint8_t i = 0; i < numOfArrays; ++i) {
            if (remain < 3) break;
            uint8_t nal_unit_type = p[0] & 0x3F;
            uint16_t numNalus = (p[1] << 8) | p[2];
            p += 3; remain -= 3;
            for (uint16_t j = 0; j < numNalus; ++j) {
                if (remain < 2) break;
                size_t n = get16(p);
                p += 2; remain -= 2;
                if (remain < n) break;
                // nal_unit_type 33=SPS, 34=PPS, 32=VPS
                out.emplace_back(p, p + n);
                p += n; remain -= n;
            }
        }
        return out;
    }

    uint32_t GetH264_MaxDpbMbs(uint8_t level_idc) {
        switch (level_idc) {
        case 10: return 396;
        case 11: return 900;
        case 12: case 13: case 20: return 2376;
        case 21: return 4752;
        case 22: case 30: return 8100;
        case 31: return 18000;
        case 32: return 20480;
        case 40: case 41: return 32768;
        case 42: return 34816;
        case 50: return 110400;
        case 51: case 52: default: return 184320;
        }
    }

    UINT CalcNumDpbSlotsFromH264SPS_RBSP(const std::vector<uint8_t>& sps_rbsp) {
        BitReader br{ sps_rbsp.data(), sps_rbsp.size(), 0 };
        // NAL header(8)はAnnexB NALの先頭に含まれるが、RBSPは通常NAL除去済み扱い。
        // ここではRBSP前提なのでNALヘッダ読みはしない。

        uint8_t profile_idc = br.readBits(8);
        br.readBits(8); // constraint flags
        uint8_t level_idc = br.readBits(8);
        br.readUE(); // seq_parameter_set_id

        if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
            profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
            profile_idc == 128 || profile_idc == 138 || profile_idc == 144) {
            uint32_t chroma_format_idc = br.readUE();
            if (chroma_format_idc == 3) br.readBits(1);
            br.readUE(); // bit_depth_luma_minus8
            br.readUE(); // bit_depth_chroma_minus8
            br.readBits(1); // qpprime_y_zero_transform_bypass_flag
            if (br.readBits(1)) {
                // seq_scaling_matrix_present_flag（スキップ）
            }
        }

        uint32_t log2_max_frame_num_minus4 = br.readUE(); (void)log2_max_frame_num_minus4;
        uint32_t pic_order_cnt_type = br.readUE();
        if (pic_order_cnt_type == 0) br.readUE();
        else if (pic_order_cnt_type == 1) {
            br.readBits(1); br.readSE(); br.readSE();
            uint32_t n = br.readUE();
            for (uint32_t i = 0; i < n; ++i) br.readSE();
        }

        uint32_t max_num_ref_frames = br.readUE();
        br.readBits(1); // gaps_in_frame_num_value_allowed_flag
        uint32_t pic_width_in_mbs_minus1 = br.readUE();
        uint32_t pic_height_in_map_units_minus1 = br.readUE();
        uint32_t frame_mbs_only_flag = br.readBits(1);
        if (!frame_mbs_only_flag) br.readBits(1);
        br.readBits(1); // direct_8x8_inference_flag
        uint32_t frame_cropping_flag = br.readBits(1);
        if (frame_cropping_flag) {
            br.readUE(); br.readUE(); br.readUE(); br.readUE();
        }

        uint32_t PicWidthInMbs = pic_width_in_mbs_minus1 + 1;
        uint32_t PicHeightInMapUnits = pic_height_in_map_units_minus1 + 1;
        uint32_t FrameHeightInMbs = (2 - frame_mbs_only_flag) * PicHeightInMapUnits;
        uint32_t PicSizeInMbs = PicWidthInMbs * FrameHeightInMbs;

        uint32_t MaxDpbMbs = GetH264_MaxDpbMbs(level_idc);
        uint32_t maxRefByLevel = max(1u, MaxDpbMbs / PicSizeInMbs);
        uint32_t dpb = min(max_num_ref_frames, maxRefByLevel);

        // デコード中フレーム分を1足す実装もある（必要なら+1）
        return (UINT)std::clamp<uint32_t>(dpb, 1, 16);
    }

    struct HEVC_SPS_Parsed {
        uint8_t general_level_idc{};
        uint32_t sps_max_sub_layers_minus1{};
        std::vector<uint32_t> sps_max_dec_pic_buffering_minus1; // size = sps_max_sub_layers_minus1+1
        uint32_t width{};
        uint32_t height{};
    };

    bool ParseHEVC_SPS_RBSP(const std::vector<uint8_t>& sps_rbsp, HEVC_SPS_Parsed& out) {
        BitReader br{ sps_rbsp.data(), sps_rbsp.size(), 0 };
        // profile_tier_level の先頭には sps_video_parameter_set_id 等がある
        br.readBits(4); // sps_video_parameter_set_id
        out.sps_max_sub_layers_minus1 = br.readBits(3);
        br.readBits(1); // sps_temporal_id_nesting_flag

        // profile_tier_level
        br.readBits(2); // profile_space + tier_flag
        br.readBits(5); // profile_idc
        br.readBits(32); // profile_compatibility_flags
        br.readBits(48); // constraint flags など
        out.general_level_idc = (uint8_t)br.readBits(8);

        // sub_layer_profile_present_flag と sub_layer_level_present_flag
        std::vector<uint8_t> splp(out.sps_max_sub_layers_minus1);
        std::vector<uint8_t> sllp(out.sps_max_sub_layers_minus1);
        for (uint32_t i = 0; i < out.sps_max_sub_layers_minus1; ++i) {
            splp[i] = (uint8_t)br.readBits(1);
            sllp[i] = (uint8_t)br.readBits(1);
        }
        if (out.sps_max_sub_layers_minus1) {
            for (uint32_t i = out.sps_max_sub_layers_minus1; i < 8; ++i) br.readBits(2); // reserved
        }
        for (uint32_t i = 0; i < out.sps_max_sub_layers_minus1; ++i) {
            if (splp[i]) {
                br.readBits(2 + 5 + 32 + 48); // sub_layer_profile_* 略
            }
            if (sllp[i]) {
                br.readBits(8); // sub_layer_level_idc
            }
        }

        br.readUE(); // sps_seq_parameter_set_id
        uint32_t chroma_format_idc = br.readUE();
        if (chroma_format_idc == 3) br.readBits(1);

        uint32_t pic_width_in_luma_samples = br.readUE();
        uint32_t pic_height_in_luma_samples = br.readUE();
        out.width = pic_width_in_luma_samples;
        out.height = pic_height_in_luma_samples;

        uint32_t conformance_window_flag = br.readBits(1);
        if (conformance_window_flag) {
            br.readUE(); br.readUE(); br.readUE(); br.readUE(); // left, right, top, bottom
        }
        br.readUE(); // bit_depth_luma_minus8
        br.readUE(); // bit_depth_chroma_minus8
        br.readUE(); // log2_max_pic_order_cnt_lsb_minus4

        uint32_t sps_sub_layer_ordering_info_present_flag = br.readBits(1);
        uint32_t startLayer = sps_sub_layer_ordering_info_present_flag ? 0 : out.sps_max_sub_layers_minus1;
        out.sps_max_dec_pic_buffering_minus1.resize(out.sps_max_sub_layers_minus1 + 1);
        for (uint32_t i = startLayer; i <= out.sps_max_sub_layers_minus1; ++i) {
            out.sps_max_dec_pic_buffering_minus1[i] = br.readUE();
            br.readUE(); // sps_max_num_reorder_pics
            br.readUE(); // sps_max_latency_increase_plus1
        }

        return true;
    }

    // HEVC Levelに応じたMaxDpbSize（簡易近似）
    uint32_t HEVC_ApproxMaxDpbSize(uint8_t general_level_idc, uint32_t width, uint32_t height) {
        // 実仕様は MaxLumaPs と MaxDpbPicBuf から導出、最終的に最大16。
        // ここでは実運用で安全な近似として、最大16、低レベルかつ大解像度で少し絞る程度。
        const uint64_t pixels = uint64_t(width) * uint64_t(height);
        // levelごとのMaxLumaPs（代表値の近似）
        uint64_t maxLumaPs =
            (general_level_idc <= 30) ? 36864ULL :          // <= 176x144
            (general_level_idc <= 31) ? 122880ULL :         // CIFあたり
            (general_level_idc <= 40) ? 2228224ULL :        // 1080p未満
            (general_level_idc <= 41) ? 2228224ULL :        // 1080p
            (general_level_idc <= 50) ? 8912896ULL :        // 4K未満
            (general_level_idc <= 51) ? 35651584ULL :       // 4K
            35651584ULL;                                    // 4K+
        // DPB上限はだいたい「max 16」。画素比から少し抑制
        uint32_t byPixels = (pixels == 0) ? 1u : (uint32_t)std::clamp<uint64_t>(maxLumaPs / std::max<uint64_t>(pixels, 1), 1, 16);
        return std::clamp<uint32_t>(byPixels, 1, 16);
    }

    UINT CalcNumDpbSlotsFromHEVC_SPS_RBSP(const std::vector<uint8_t>& sps_rbsp) {
        HEVC_SPS_Parsed s{};
        if (!ParseHEVC_SPS_RBSP(sps_rbsp, s)) return 8; // フォールバック
        uint32_t highest = s.sps_max_sub_layers_minus1;
        uint32_t decPicBuf = 0;
        if (!s.sps_max_dec_pic_buffering_minus1.empty() && highest < s.sps_max_dec_pic_buffering_minus1.size()) {
            decPicBuf = s.sps_max_dec_pic_buffering_minus1[highest] + 1;
        } else if (!s.sps_max_dec_pic_buffering_minus1.empty()) {
            decPicBuf = s.sps_max_dec_pic_buffering_minus1.back() + 1;
        } else {
            decPicBuf = 6; // safe default
        }
        uint32_t cap = HEVC_ApproxMaxDpbSize(s.general_level_idc, s.width, s.height);
        uint32_t dpb = std::min<uint32_t>(decPicBuf, cap);
        return (UINT)std::clamp<uint32_t>(dpb, 1, 16);
    }

    UINT CalcNumDpbSlotsFromVP9(uint32_t /*profile*/, uint32_t /*width*/, uint32_t /*height*/) {
        return 8; // 実装上の安全上限
    }

    UINT CalcNumDpbSlotsFromAV1(uint32_t /*operatingPointIdc*/, bool /*enableReorder*/) {
        // デフォルト8。高品質プロファイルやSVCで余裕が欲しければ12に。
        return 8;
    }

    bool GetAttributeBlob(IMFMediaType* mt, const GUID& key, std::vector<uint8_t>& out) {
        UINT32 cb = 0;
        UINT8* p = nullptr;
        HRESULT hr = mt->GetAllocatedBlob(key, &p, &cb);
        if (FAILED(hr) || !p || !cb) return false;
        out.assign(p, p + cb);
        CoTaskMemFree(p);
        return true;
    }

    bool ExtractH264_SPS_FromMF(IMFSourceReader* reader, std::vector<uint8_t>& sps_rbsp_out) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) return false;

        std::vector<uint8_t> blob;
        if (!GetAttributeBlob(mt.Get(), MF_MT_MPEG_SEQUENCE_HEADER, blob)) return false;

        // AnnexB or AVCC を判定
        bool looksAnnexB = blob.size() >= 4 &&
            ((blob[0] == 0 && blob[1] == 0 && blob[2] == 1) || (blob[0] == 0 && blob[1] == 0 && blob[2] == 0 && blob[3] == 1));
        std::vector<std::vector<uint8_t>> nalus;
        if (looksAnnexB) {
            nalus = SplitAnnexB(blob.data(), blob.size());
        } else {
            nalus = ParseAVCDecoderConfigurationRecord(blob.data(), blob.size());
        }

        // SPS(= nal_type 7)を探す
        for (auto& n : nalus) {
            if (n.empty()) continue;
            uint8_t nal_hdr = n[0];
            uint8_t nal_type = nal_hdr & 0x1F;
            if (nal_type == 7) {
                auto rbsp = ToRBSP(n.data(), n.size());
                sps_rbsp_out = std::move(rbsp);
                return true;
            }
        }
        return false;
    }

    bool ExtractHEVC_SPS_FromMF(IMFSourceReader* reader, std::vector<uint8_t>& sps_rbsp_out) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))) return false;

        std::vector<uint8_t> blob;
        if (!GetAttributeBlob(mt.Get(), MF_MT_MPEG_SEQUENCE_HEADER, blob)) return false;

        bool looksAnnexB = blob.size() >= 4 &&
            ((blob[0] == 0 && blob[1] == 0 && blob[2] == 1) || (blob[0] == 0 && blob[1] == 0 && blob[2] == 0 && blob[3] == 1));
        std::vector<std::vector<uint8_t>> nalus;
        if (looksAnnexB) {
            nalus = SplitAnnexB(blob.data(), blob.size());
        } else {
            nalus = ParseHEVCDecoderConfigurationRecord(blob.data(), blob.size());
        }

        // SPS(= nal_unit_type 33)を探す
        for (auto& n : nalus) {
            if (n.empty()) continue;
            uint8_t h = n[0];
            uint8_t nal_unit_type = (h >> 1) & 0x3F;
            if (nal_unit_type == 33) {
                auto rbsp = ToRBSP(n.data(), n.size());
                sps_rbsp_out = std::move(rbsp);
                return true;
            }
        }
        return false;
    }


    UINT ComputeDpbSlots(IMFSourceReader* reader, const GUID& subtype, uint32_t widthHint = 0, uint32_t heightHint = 0) {
        if (subtype == MFVideoFormat_H264) {
            std::vector<uint8_t> sps_rbsp;
            if (ExtractH264_SPS_FromMF(reader, sps_rbsp)) {
                return CalcNumDpbSlotsFromH264SPS_RBSP(sps_rbsp);
            }
            // フォールバック（1080p以下なら4〜6、余裕を見て6）
            return 6;
        }
        if (subtype == MFVideoFormat_HEVC) {
            std::vector<uint8_t> sps_rbsp;
            if (ExtractHEVC_SPS_FromMF(reader, sps_rbsp)) {
                return CalcNumDpbSlotsFromHEVC_SPS_RBSP(sps_rbsp);
            }
            // フォールバック：解像度で軽く分岐
            uint32_t w = widthHint, h = heightHint;
            if (w * h <= 1920u * 1080u) return 8;
            else return 12;
        }
        if (subtype == MFVideoFormat_VP90) {
            return CalcNumDpbSlotsFromVP9(0, widthHint, heightHint); // profile等あれば渡す
        }
        if (subtype == MFVideoFormat_AV1) {
            return CalcNumDpbSlotsFromAV1(0, /*enableReorder*/true);
        }
        // 不明コーデックは保守的に4
        return 4;
    }



    inline size_t Align(size_t size, UINT align) {
        return size_t(size + align - 1) & ~(size_t(align) - 1);
    }


    bool GPUMovie::Load(YRZ::DXR* dxr, const fs::path& filename, bool& fallbackNeeded)
    {
        m_dxr = dxr;
        auto pathw = filename.wstring();
        fallbackNeeded = false;

        // MF
        ThrowIfFailed(MFCreateSourceReaderFromURL(pathw.c_str(), nullptr, &m_reader), "GPUMovie::Load Failed (MFCreateSourceReaderFromURL)");
        m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        // 1) ストリーム情報取得
        StreamInfo si{};
        if (!GetStreamInfo(m_reader.Get(), si)) return false;

        //サイズの取得
        ComPtr<IMFMediaType> pMediaType;
        auto hr = m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, pMediaType.GetAddressOf());
        if (SUCCEEDED(hr)) {
            hr = MFGetAttributeSize(pMediaType.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height);
        }

        //再生時間の取得
        LONGLONG duration = 0;
        PROPVARIANT var;
        hr = m_reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
        if (SUCCEEDED(hr)) {
            duration = var.uhVal.QuadPart;
            PropVariantClear(&var);
            m_duration = (double)duration / 1e+7;
        }

        //フレームレート
        uint32_t numerator, denominator;
        hr = MFGetAttributeRatio(pMediaType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
        if (SUCCEEDED(hr)) {
            m_fps = (double)numerator / denominator;
        }

        // 2) D3D12/VideoDevice
        ThrowIfFailed(dxr->Device().As(&m_videoDevice), "GPUMovie::Load Failed (dxr->Device().As)");

        // 3) コーデック→候補生成
        auto cand = MakeCandidatesFromSubtype(si.subtype);

        // 4) CheckFeatureSupport で選択
        ChosenDecode chosen{};
        bool supported = PickDecoderConfig(m_videoDevice.Get(), si, cand, chosen);

        // RTX 2070 の AV1 等、非対応はここで false になる。WMV(VC-1) はドライバによって非対応の場合あり。
        if (!supported) {
            fallbackNeeded = true; // 下でフォールバック経路へ
        } else {
            // 5) Video デコード用キュー/コマンド
            D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
            ThrowIfFailed(dxr->Device()->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQ)), "GPUMovie::Load Failed (CreateCommandQueue)");
            ThrowIfFailed(dxr->Device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, IID_PPV_ARGS(&m_cmdAlloc)), "GPUMovie::Load Failed (CreateCommandAllocator)");
            ThrowIfFailed(dxr->Device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "GPUMovie::Load Failed (CreateCommandList)");

            // 6) デコーダ/ヒープ
            UINT nDBP = ComputeDpbSlots(m_reader.Get(), m_subtype, m_width, m_height);
            ThrowIfFailed(CreateDecoderAndHeap(m_videoDevice.Get(), chosen, nDBP, m_decoder, m_heap), "GPUMovie::Load Failed (CreateDecoderAndHeap)");

            m_width = chosen.width;
            m_height = chosen.height;
            m_format = chosen.surfaceFormat;
            m_subtype = si.subtype;
            m_useD3D12 = true;

            //7)リソースの作成
            Output = {};
            
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Format = m_format;
            desc.Width = m_width;
            desc.Height = m_height;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.DepthOrArraySize = nDBP;
            desc.SampleDesc.Count = 1;
            desc.MipLevels = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            
            auto hprop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            dxr->Device()->CreateCommittedResource(&hprop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Output.res));
            Output.type = YRZ::ResType::tex2D;
            Output.caps = YRZ::ResCaps::srv;
            Output.SetName(L"Wave::GPUMovie::VideoTextureArray");

            //仮に4Bytes/pixが最大とする
            m_frameSize = Align(size_t(m_width) * m_height * 4, D3D12_VIDEO_DECODE_MIN_BITSTREAM_OFFSET_ALIGNMENT);
            m_upload = dxr->CreateBufCPU(nullptr, 1, m_frameSize, true, false);
            m_upload.SetName(L"Wave::GPUMovie::m_upload");

            YRZ::DEB8("video load success, width:{} height:{} format:{} nDBP:{}", m_width, m_height, YRZ::u8(YRZ::DXGIFormatToString(m_format)), nDBP);
        }

        // 以降: reader は demux 用に保持。実運用では elementary stream の取り出し/パースを実装する。
        // H.264/HEVC/VP9/AV1 は AnnexB or MP4/ISOBMFF 由来の NALU/OBU パースが必要。
        return true;
    }

    bool GPUMovie::GetFrame(double time)
    {
        /*
        LONGLONG target = (LONGLONG)(min(m_duration, max(0, time)) * 1e+7); //はみ出てる範囲が指定された場合は納める
        LONGLONG frameT = (LONGLONG)(1e+7 / m_fps); //1フレーム分の時間
        LONGLONG lastT = (LONGLONG)(m_duration * 1e+7); //動画の終わり

        //前回GetFrameできた時刻と、指定された時刻の差がフレームレート未満だったら帰る
        if (m_prevT >= 0 && m_prevT <= target && target < (m_prevT + frameT)) {
            return false;
        }

        //前回GetFrameした時刻から0.5秒以下だったらシークせず、ReadSampleで順送りする(GOPの頻度が分からないので)
        //逆再生の場合はシークする
        if (!(target > m_prevT && (target - m_prevT <= frameT * m_fps / 2))) {
            PROPVARIANT var;
            InitPropVariantFromInt64(target, &var);
            ThrowIfFailed(m_reader->SetCurrentPosition(GUID_NULL, var), "m_reader->SetCurrentPosition failed");
            PropVariantClear(&var);
        }


        DWORD streamIndex, flags;
        LONGLONG timestamp;
        ComPtr<IMFSample> pSample;
        int cacheIndex = 0;
        //シークされた地点から順送り
        while (1) {
            ThrowIfFailed(m_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0, &streamIndex, &flags, &timestamp, pSample.ReleaseAndGetAddressOf()), "m_reader->ReadSample failed");
            //目標点か動画の最後まで来たら順送り終了
            if (timestamp + frameT >= target)
                break;
            if (timestamp >= lastT)
                break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                break;
        }

        if (pSample) {
            ComPtr<IMFMediaBuffer> pBuffer;
            ThrowIfFailed(pSample->ConvertToContiguousBuffer(pBuffer.ReleaseAndGetAddressOf()), "pSample->ConvertToContiguousBuffer failed");

            BYTE* pData = nullptr;
            DWORD maxLength = 0, currentLength = 0;
            auto hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
            if (SUCCEEDED(hr)) {
                //アップロード用バッファにデコード前データをコピー
                std::uint8_t* pup;
                m_upload.res->Map(0, nullptr, (void**)&pup);
                memcpy_s(pup, m_frameSize, pData, min(currentLength, m_frameSize));
                pBuffer->Unlock();
                m_upload.res->Unmap(0, nullptr);

                // DecodeFrame引数設定
                D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS inArgs = {};
                inArgs.NumFrameArguments = 2;

                inArgs.FrameArguments[0].Type = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS;
                inArgs.FrameArguments[0].Size = sizeof(picParams);
                inArgs.FrameArguments[0].pData = &picParams;

                inArgs.FrameArguments[1].Type = D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL;
                inArgs.FrameArguments[1].Size = sizeof(sliceCtrl);
                inArgs.FrameArguments[1].pData = &sliceCtrl;

                inArgs.ReferenceFrames.NumTexture2Ds = Output.desc().DepthOrArraySize;
                inArgs.ReferenceFrames.ppHeaps = 

                inArgs.CompressedBitstream.pBuffer = m_upload.res.Get();
                inArgs.CompressedBitstream.Offset = 0;
                inArgs.CompressedBitstream.Size = currentLength;
                inArgs.pHeap = m_heap.Get();

                D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS outArgs = {};
                outArgs.pOutputTexture2D = Output.res.Get();
                outArgs.OutputSubresource = 0;

                //コマンド実行
                m_cmdAlloc->Reset();
                m_cmdList->Reset(m_cmdAlloc.Get());
                m_cmdList->DecodeFrame(m_decoder.Get(), &outArgs, &inArgs);
                m_cmdList->Close();

                ID3D12CommandList* lists[] = { m_cmdList.Get() };
                m_cmdQ->ExecuteCommandLists(1, lists);
                WaitForGPU();
            }
            m_prevT = timestamp;
        }
        */
        return true;
    }

}



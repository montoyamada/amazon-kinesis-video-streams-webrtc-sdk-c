#include "Samples.h"
#include <alsa/asoundlib.h>

extern PSampleConfiguration gSampleConfiguration;

// Audio handling thread and state
static TID gAudioCaptureThread = INVALID_TID_VALUE;
static TID gAudioPlaybackThread = INVALID_TID_VALUE;
static volatile ATOMIC_BOOL gTerminateAudio = FALSE;
static PSampleStreamingSession gStreamingSession = NULL;

// ALSA devices
static snd_pcm_t* gCaptureHandle = NULL;
static snd_pcm_t* gPlaybackHandle = NULL;

// Audio buffer for playback
typedef struct {
    PBYTE buffer;
    UINT32 size;
    UINT64 pts;
} AudioFrame;

static PStackQueue gAudioPlaybackQueue = NULL;
static MUTEX gAudioQueueLock;

// Audio frame handler for receiving audio from peer
VOID viewerAudioFrameHandler(UINT64 customData, PFrame pFrame)
{
    STATUS retStatus = STATUS_SUCCESS;
    AudioFrame* pAudioFrame = NULL;
    UINT64 queueSize = 0;
    
    UNUSED_PARAM(customData);
    
    if (pFrame != NULL && pFrame->size > 0) {
        // Create audio frame for playback queue
        pAudioFrame = (AudioFrame*) MEMALLOC(SIZEOF(AudioFrame));
        CHK(pAudioFrame != NULL, STATUS_NOT_ENOUGH_MEMORY);
        
        pAudioFrame->buffer = (PBYTE) MEMALLOC(pFrame->size);
        CHK(pAudioFrame->buffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
        
        MEMCPY(pAudioFrame->buffer, pFrame->frameData, pFrame->size);
        pAudioFrame->size = pFrame->size;
        pAudioFrame->pts = pFrame->presentationTs;
        
        // Add to playback queue
        MUTEX_LOCK(gAudioQueueLock);
        CHK_STATUS(stackQueueGetCount(gAudioPlaybackQueue, &queueSize));
        
        // Limit queue size to prevent unbounded growth
        if (queueSize < 100) {
            CHK_STATUS(stackQueueEnqueue(gAudioPlaybackQueue, (UINT64) pAudioFrame));
            pAudioFrame = NULL; // Ownership transferred
        } else {
            DLOGW("[KVS Viewer] Audio playback queue full, dropping frame");
        }
        MUTEX_UNLOCK(gAudioQueueLock);
    }
    
CleanUp:
    if (pAudioFrame != NULL) {
        if (pAudioFrame->buffer != NULL) {
            MEMFREE(pAudioFrame->buffer);
        }
        MEMFREE(pAudioFrame);
    }
}

// Get the best available audio device (prefer USB, avoid HDMI)
PCHAR getBestAudioDevice(BOOL isPlayback)
{
    FILE* fp;
    CHAR buffer[256];
    CHAR command[512];
    static CHAR selectedDevice[64];
    
    // Default to plughw for automatic format conversion
    STRCPY(selectedDevice, "plughw:0,0");
    
    // Check for USB audio devices first
    if (isPlayback) {
        STRCPY(command, "aplay -l | grep -i usb | head -1");
    } else {
        STRCPY(command, "arecord -l | grep -i usb | head -1");
    }
    
    fp = popen(command, "r");
    if (fp != NULL) {
        if (fgets(buffer, sizeof(buffer), fp) != NULL) {
            // Parse "card X: device Y:" format
            PCHAR cardStr = strstr(buffer, "card ");
            PCHAR deviceStr = strstr(buffer, "device ");
            if (cardStr != NULL && deviceStr != NULL) {
                INT32 card = atoi(cardStr + 5);
                INT32 device = atoi(deviceStr + 7);
                snprintf(selectedDevice, sizeof(selectedDevice), "plughw:%d,%d", card, device);
                pclose(fp);
                DLOGI("[KVS Viewer] Selected USB audio device: %s", selectedDevice);
                return selectedDevice;
            }
        }
        pclose(fp);
    }
    
    DLOGI("[KVS Viewer] Using analog audio device: %s", selectedDevice);
    return selectedDevice;
}

// Initialize ALSA capture device
STATUS initializeAlsaCapture(snd_pcm_t** ppHandle, UINT32 sampleRate, UINT32 channels, UINT32 periodSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    INT32 err;
    snd_pcm_hw_params_t* hwParams = NULL;
    PCHAR deviceName = getBestAudioDevice(FALSE);
    
    // Open PCM device for capture - Using best available device
    err = snd_pcm_open(ppHandle, deviceName, SND_PCM_STREAM_CAPTURE, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Allocate hardware parameters
    err = snd_pcm_hw_params_malloc(&hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Initialize hardware parameters
    err = snd_pcm_hw_params_any(*ppHandle, hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set access type
    err = snd_pcm_hw_params_set_access(*ppHandle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set sample format (16-bit signed little-endian)
    err = snd_pcm_hw_params_set_format(*ppHandle, hwParams, SND_PCM_FORMAT_S16_LE);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set channels
    err = snd_pcm_hw_params_set_channels(*ppHandle, hwParams, channels);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set sample rate
    err = snd_pcm_hw_params_set_rate_near(*ppHandle, hwParams, &sampleRate, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set period size
    err = snd_pcm_hw_params_set_period_size_near(*ppHandle, hwParams, &periodSize, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Apply hardware parameters
    err = snd_pcm_hw_params(*ppHandle, hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Prepare the device
    err = snd_pcm_prepare(*ppHandle);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    DLOGI("[KVS Viewer] ALSA capture device initialized: rate=%u, channels=%u", sampleRate, channels);
    
CleanUp:
    if (hwParams != NULL) {
        snd_pcm_hw_params_free(hwParams);
    }
    
    if (STATUS_FAILED(retStatus) && *ppHandle != NULL) {
        snd_pcm_close(*ppHandle);
        *ppHandle = NULL;
    }
    
    return retStatus;
}

// Thread function for capturing audio from microphone
PVOID audioCaptureThread(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    Frame frame;
    UINT32 sampleRate = 48000; // 48 kHz - Standard WebRTC rate for JS compatibility
    UINT32 channels = 2;
    UINT32 samplesPerFrame = 960; // 20ms of audio at 48kHz
    UINT32 bytesPerSample = 2; // 16-bit samples
    UINT32 frameSize = samplesPerFrame * channels * bytesPerSample;
    PBYTE audioBuffer = NULL;
    UINT64 timestamp = 0;
    UINT64 frameCount = 0;
    INT32 frames;
    
    audioBuffer = (PBYTE) MEMALLOC(frameSize);
    CHK(audioBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY);
    
    // Initialize ALSA capture
    CHK_STATUS(initializeAlsaCapture(&gCaptureHandle, sampleRate, channels, samplesPerFrame));
    
    DLOGI("[KVS Viewer] Audio capture thread started");
    
    // Capture and send audio
    while (!ATOMIC_LOAD_BOOL(&gTerminateAudio) && 
           !ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag)) {
        
        // Read audio from microphone
        frames = snd_pcm_readi(gCaptureHandle, audioBuffer, samplesPerFrame);
        
        if (frames < 0) {
            // Handle underrun
            if (frames == -EPIPE) {
                DLOGW("[KVS Viewer] ALSA capture underrun, recovering...");
                snd_pcm_prepare(gCaptureHandle);
                continue;
            } else {
                DLOGE("[KVS Viewer] ALSA read error: %s", snd_strerror(frames));
                break;
            }
        }
        
        if (frames > 0) {
            // Create frame
            frame.frameData = audioBuffer;
            frame.size = frames * channels * bytesPerSample;
            frame.presentationTs = timestamp;
            frame.decodingTs = timestamp;
            frame.duration = SAMPLE_AUDIO_FRAME_DURATION;
            frame.index = frameCount++;
            frame.flags = FRAME_FLAG_NONE;
            
            // Send frame if audio transceiver is ready
            if (pSampleStreamingSession->pAudioRtcRtpTransceiver != NULL) {
                retStatus = writeFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, &frame);
                if (retStatus != STATUS_SUCCESS) {
                    // Only log non-SRTP "not ready" errors to reduce noise during initialization
                    if (retStatus != STATUS_SRTP_NOT_READY_YET) {
                        DLOGW("[KVS Viewer] writeFrame() failed with 0x%08x", retStatus);
                    }
                }
            }
            
            timestamp += SAMPLE_AUDIO_FRAME_DURATION;
        }
    }
    
CleanUp:
    
    DLOGI("[KVS Viewer] Audio capture thread terminated");
    
    if (gCaptureHandle != NULL) {
        snd_pcm_close(gCaptureHandle);
        gCaptureHandle = NULL;
    }
    
    if (audioBuffer != NULL) {
        MEMFREE(audioBuffer);
    }
    
    return (PVOID) (ULONG_PTR) retStatus;
}

// Initialize ALSA playback device
STATUS initializeAlsaPlayback(snd_pcm_t** ppHandle, UINT32 sampleRate, UINT32 channels, UINT32 periodSize)
{
    STATUS retStatus = STATUS_SUCCESS;
    INT32 err;
    snd_pcm_hw_params_t* hwParams = NULL;
    PCHAR deviceName = getBestAudioDevice(TRUE);
    
    // Open PCM device for playback - Using best available device
    err = snd_pcm_open(ppHandle, deviceName, SND_PCM_STREAM_PLAYBACK, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Allocate hardware parameters
    err = snd_pcm_hw_params_malloc(&hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Initialize hardware parameters
    err = snd_pcm_hw_params_any(*ppHandle, hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set access type
    err = snd_pcm_hw_params_set_access(*ppHandle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set sample format (16-bit signed little-endian)
    err = snd_pcm_hw_params_set_format(*ppHandle, hwParams, SND_PCM_FORMAT_S16_LE);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set channels
    err = snd_pcm_hw_params_set_channels(*ppHandle, hwParams, channels);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set sample rate
    err = snd_pcm_hw_params_set_rate_near(*ppHandle, hwParams, &sampleRate, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Set period size
    err = snd_pcm_hw_params_set_period_size_near(*ppHandle, hwParams, &periodSize, 0);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Apply hardware parameters
    err = snd_pcm_hw_params(*ppHandle, hwParams);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    // Prepare the device
    err = snd_pcm_prepare(*ppHandle);
    CHK(err >= 0, STATUS_INTERNAL_ERROR);
    
    DLOGI("[KVS Viewer] ALSA playback device initialized: rate=%u, channels=%u", sampleRate, channels);
    
CleanUp:
    if (hwParams != NULL) {
        snd_pcm_hw_params_free(hwParams);
    }
    
    if (STATUS_FAILED(retStatus) && *ppHandle != NULL) {
        snd_pcm_close(*ppHandle);
        *ppHandle = NULL;
    }
    
    return retStatus;
}

// Thread function for playing audio to speaker
PVOID audioPlaybackThread(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 sampleRate = 48000; // 48 kHz - EKSA device supports this
    UINT32 channels = 2;
    UINT32 samplesPerFrame = 960; // 20ms of audio at 48kHz
    UINT32 bytesPerSample = 2; // 16-bit samples
    AudioFrame* pAudioFrame = NULL;
    INT32 frames;
    UINT64 item;
    
    UNUSED_PARAM(args);
    
    // Initialize ALSA playback
    CHK_STATUS(initializeAlsaPlayback(&gPlaybackHandle, sampleRate, channels, samplesPerFrame));
    
    DLOGI("[KVS Viewer] Audio playback thread started");
    
    // Play audio from queue
    while (!ATOMIC_LOAD_BOOL(&gTerminateAudio)) {
        // Get frame from queue
        MUTEX_LOCK(gAudioQueueLock);
        retStatus = stackQueueDequeue(gAudioPlaybackQueue, &item);
        MUTEX_UNLOCK(gAudioQueueLock);
        
        if (STATUS_SUCCEEDED(retStatus)) {
            pAudioFrame = (AudioFrame*) item;
            
            // Write audio to speaker
            frames = snd_pcm_writei(gPlaybackHandle, pAudioFrame->buffer, 
                                    pAudioFrame->size / (channels * bytesPerSample));
            
            if (frames < 0) {
                // Handle underrun
                if (frames == -EPIPE) {
                    DLOGW("[KVS Viewer] ALSA playback underrun, recovering...");
                    snd_pcm_prepare(gPlaybackHandle);
                } else {
                    DLOGE("[KVS Viewer] ALSA write error: %s", snd_strerror(frames));
                }
            }
            
            // Free the audio frame
            MEMFREE(pAudioFrame->buffer);
            MEMFREE(pAudioFrame);
            pAudioFrame = NULL;
        } else {
            // No audio in queue, sleep briefly
            THREAD_SLEEP(10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        }
    }
    
CleanUp:
    
    DLOGI("[KVS Viewer] Audio playback thread terminated");
    
    if (gPlaybackHandle != NULL) {
        snd_pcm_close(gPlaybackHandle);
        gPlaybackHandle = NULL;
    }
    
    // Clean up any remaining frames in queue
    if (gAudioPlaybackQueue != NULL) {
        MUTEX_LOCK(gAudioQueueLock);
        while (STATUS_SUCCEEDED(stackQueueDequeue(gAudioPlaybackQueue, &item))) {
            pAudioFrame = (AudioFrame*) item;
            MEMFREE(pAudioFrame->buffer);
            MEMFREE(pAudioFrame);
        }
        MUTEX_UNLOCK(gAudioQueueLock);
    }
    
    return (PVOID) (ULONG_PTR) retStatus;
}

#ifdef ENABLE_DATA_CHANNEL

// onMessage callback for a message received by the viewer on a data channel
VOID dataChannelOnMessageCallback(UINT64 customData, PRtcDataChannel pDataChannel, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen)
{
    UNUSED_PARAM(customData);
    UNUSED_PARAM(pDataChannel);
    if (isBinary) {
        DLOGI("DataChannel Binary Message");
    } else {
        DLOGI("DataChannel String Message: %.*s", pMessageLen, pMessage);
    }
}

// onOpen callback for the onOpen event of a viewer created data channel
VOID dataChannelOnOpenCallback(UINT64 customData, PRtcDataChannel pDataChannel)
{
    STATUS retStatus = STATUS_SUCCESS;
    DLOGI("New DataChannel has been opened %s ", pDataChannel->name);
    dataChannelOnMessage(pDataChannel, customData, dataChannelOnMessageCallback);
    ATOMIC_INCREMENT((PSIZE_T) customData);
    // Sending first message to the master over the data channel
    retStatus = dataChannelSend(pDataChannel, FALSE, (PBYTE) VIEWER_DATA_CHANNEL_MESSAGE, STRLEN(VIEWER_DATA_CHANNEL_MESSAGE));
    if (retStatus != STATUS_SUCCESS) {
        DLOGI("[KVS Viewer] dataChannelSend(): operation returned status code: 0x%08x ", retStatus);
    }
}
#endif

// Start audio handling
STATUS startAudioHandling(PSampleStreamingSession pSampleStreamingSession)
{
    STATUS retStatus = STATUS_SUCCESS;
    
    CHK(pSampleStreamingSession != NULL, STATUS_NULL_ARG);
    
    gStreamingSession = pSampleStreamingSession;
    ATOMIC_STORE_BOOL(&gTerminateAudio, FALSE);
    
    // Create audio playback queue
    CHK_STATUS(stackQueueCreate(&gAudioPlaybackQueue));
    
    // Initialize mutex for queue
    gAudioQueueLock = MUTEX_CREATE(FALSE);
    CHK(IS_VALID_MUTEX_VALUE(gAudioQueueLock), STATUS_INVALID_OPERATION);
    
    // Create audio capture thread
    CHK_STATUS(THREAD_CREATE(&gAudioCaptureThread, audioCaptureThread, (PVOID) pSampleStreamingSession));
    
    // Create audio playback thread
    CHK_STATUS(THREAD_CREATE(&gAudioPlaybackThread, audioPlaybackThread, NULL));
    
    DLOGI("[KVS Viewer] Audio handling started");
    
CleanUp:
    if (STATUS_FAILED(retStatus)) {
        stopAudioHandling();
    }
    return retStatus;
}

// Stop audio handling
VOID stopAudioHandling()
{
    UINT64 item;
    AudioFrame* pAudioFrame;
    
    ATOMIC_STORE_BOOL(&gTerminateAudio, TRUE);
    
    // Stop capture thread
    if (IS_VALID_TID_VALUE(gAudioCaptureThread)) {
        THREAD_JOIN(gAudioCaptureThread, NULL);
        gAudioCaptureThread = INVALID_TID_VALUE;
    }
    
    // Stop playback thread
    if (IS_VALID_TID_VALUE(gAudioPlaybackThread)) {
        THREAD_JOIN(gAudioPlaybackThread, NULL);
        gAudioPlaybackThread = INVALID_TID_VALUE;
    }
    
    // Clean up queue
    if (gAudioPlaybackQueue != NULL) {
        while (STATUS_SUCCEEDED(stackQueueDequeue(gAudioPlaybackQueue, &item))) {
            pAudioFrame = (AudioFrame*) item;
            MEMFREE(pAudioFrame->buffer);
            MEMFREE(pAudioFrame);
        }
        stackQueueFree(gAudioPlaybackQueue);
        gAudioPlaybackQueue = NULL;
    }
    
    // Clean up mutex
    if (IS_VALID_MUTEX_VALUE(gAudioQueueLock)) {
        MUTEX_FREE(gAudioQueueLock);
        gAudioQueueLock = INVALID_MUTEX_VALUE;
    }
    
    gStreamingSession = NULL;
    DLOGI("[KVS Viewer] Audio handling stopped");
}

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    RtcSessionDescriptionInit offerSessionDescriptionInit;
    UINT32 buffLen = 0;
    SignalingMessage message;
    PSampleConfiguration pSampleConfiguration = NULL;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    BOOL locked = FALSE;
    PCHAR pChannelName;
    CHAR clientId[256];

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

#ifndef _WIN32
    signal(SIGINT, sigintHandler);
#endif

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    if (argc > 2) {
        if (!STRCMP(argv[2], AUDIO_CODEC_NAME_OPUS)) {
            audioCodec = RTC_CODEC_OPUS;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_ALAW)) {
            audioCodec = RTC_CODEC_ALAW;
        } else if (!STRCMP(argv[2], AUDIO_CODEC_NAME_MULAW)) {
            audioCodec = RTC_CODEC_MULAW;
        } else {
            DLOGI("[KVS Viewer] Defaulting to Opus audio codec");
        }
    }

    if (argc > 3) {
        if (!STRCMP(argv[3], VIDEO_CODEC_NAME_H265)) {
            videoCodec = RTC_CODEC_H265;
        } else if (!STRCMP(argv[3], VIDEO_CODEC_NAME_VP8)) {
            videoCodec = RTC_CODEC_VP8;
        } else {
            DLOGI("[KVS Viewer] Defaulting to H264 video codec");
        }
    }

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_VIEWER, TRUE, TRUE, logLevel, &pSampleConfiguration));
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS Viewer] KVS WebRTC initialization completed successfully");

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif

    SPRINTF(clientId, "%s_%u", SAMPLE_VIEWER_CLIENT_ID, RAND() % MAX_UINT32);
    CHK_STATUS(initSignaling(pSampleConfiguration, clientId));
    DLOGI("[KVS Viewer] Signaling client connection established");

    // Initialize streaming session
    MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = TRUE;
    CHK_STATUS(createSampleStreamingSession(pSampleConfiguration, NULL, FALSE, &pSampleStreamingSession));
    DLOGI("[KVS Viewer] Creating streaming session...completed");
    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSampleStreamingSession;

    MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    locked = FALSE;

    MEMSET(&offerSessionDescriptionInit, 0x00, SIZEOF(RtcSessionDescriptionInit));

    offerSessionDescriptionInit.useTrickleIce = pSampleStreamingSession->remoteCanTrickleIce;
    CHK_STATUS(setLocalDescription(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    DLOGI("[KVS Viewer] Completed setting local description");

    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64) pSampleStreamingSession, viewerAudioFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));

    if (!pSampleConfiguration->trickleIce) {
        DLOGI("[KVS Viewer] Non trickle ice. Wait for Candidate collection to complete");
        MUTEX_LOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = TRUE;

        while (!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->candidateGatheringDone)) {
            CHK_WARN(!ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag), STATUS_OPERATION_TIMED_OUT,
                     "application terminated and candidate gathering still not done");
            CVAR_WAIT(pSampleConfiguration->cvar, pSampleConfiguration->sampleConfigurationObjLock, 5 * HUNDREDS_OF_NANOS_IN_A_SECOND);
        }

        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
        locked = FALSE;

        DLOGI("[KVS Viewer] Candidate collection completed");
    }

    CHK_STATUS(createOffer(pSampleStreamingSession->pPeerConnection, &offerSessionDescriptionInit));
    DLOGI("[KVS Viewer] Offer creation successful");

    DLOGI("[KVS Viewer] Generating JSON of session description....");
    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, NULL, &buffLen));

    if (buffLen >= SIZEOF(message.payload)) {
        DLOGE("[KVS Viewer] serializeSessionDescriptionInit(): operation returned status code: 0x%08x ", STATUS_INVALID_OPERATION);
        retStatus = STATUS_INVALID_OPERATION;
        goto CleanUp;
    }

    CHK_STATUS(serializeSessionDescriptionInit(&offerSessionDescriptionInit, message.payload, &buffLen));

    message.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    message.messageType = SIGNALING_MESSAGE_TYPE_OFFER;
    STRCPY(message.peerClientId, SAMPLE_MASTER_CLIENT_ID);
    message.payloadLen = (buffLen / SIZEOF(CHAR)) - 1;
    message.correlationId[0] = '\0';

    CHK_STATUS(signalingClientSendMessageSync(pSampleConfiguration->signalingClientHandle, &message));
    
    // Start audio handling
    DLOGI("[KVS Viewer] Starting audio handling...");
    CHK_STATUS(startAudioHandling(pSampleStreamingSession));
    DLOGI("[KVS Viewer] Audio handling started successfully");
    
#ifdef ENABLE_DATA_CHANNEL
    PRtcDataChannel pDataChannel = NULL;
    PRtcPeerConnection pPeerConnection = pSampleStreamingSession->pPeerConnection;
    SIZE_T datachannelLocalOpenCount = 0;

    // Creating a new datachannel on the peer connection of the existing sample streaming session
    CHK_STATUS(createDataChannel(pPeerConnection, pChannelName, NULL, &pDataChannel));
    DLOGI("[KVS Viewer] Creating data channel...completed");

    // Setting a callback for when the data channel is open
    CHK_STATUS(dataChannelOnOpen(pDataChannel, (UINT64) &datachannelLocalOpenCount, dataChannelOnOpenCallback));
    DLOGI("[KVS Viewer] Data Channel open now...");
#endif

    // Block until interrupted
    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->interrupted) && !ATOMIC_LOAD_BOOL(&pSampleStreamingSession->terminateFlag)) {
        THREAD_SLEEP(HUNDREDS_OF_NANOS_IN_A_SECOND);
    }

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS Viewer] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS Viewer] Cleaning up....");
    
    // Stop audio handling
    stopAudioHandling();

    if (locked) {
        MUTEX_UNLOCK(pSampleConfiguration->sampleConfigurationObjLock);
    }

    if (pSampleConfiguration->enableFileLogging) {
        freeFileLogger();
    }
    if (pSampleConfiguration != NULL) {
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Viewer] freeSignalingClient(): operation returned status code: 0x%08x ", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Viewer] freeSampleConfiguration(): operation returned status code: 0x%08x ", retStatus);
        }
    }
    DLOGI("[KVS Viewer] Cleanup done");

    RESET_INSTRUMENTED_ALLOCATORS();

    // https://www.gnu.org/software/libc/manual/html_node/Exit-Status.html
    // We can only return with 0 - 127. Some platforms treat exit code >= 128
    // to be a success code, which might give an unintended behaviour.
    // Some platforms also treat 1 or 0 differently, so it's better to use
    // EXIT_FAILURE and EXIT_SUCCESS macros for portability.
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}

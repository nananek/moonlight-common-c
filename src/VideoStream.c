#include "Limelight-internal.h"

#define FIRST_FRAME_TIMEOUT_SEC 10

// Per-stream receive state. One of these exists per host display; the UDP
// socket, receive thread and ping thread below are shared by all of them
// because every stream is multiplexed onto the same port.
typedef struct _VIDEO_STREAM_CONTEXT {
    int streamIndex;
    RTP_VIDEO_QUEUE rtpQueue;
    PLT_THREAD decoderThread;
    bool receivedFullFrame;
} VIDEO_STREAM_CONTEXT, *PVIDEO_STREAM_CONTEXT;

static VIDEO_STREAM_CONTEXT VideoStreams[MAX_VIDEO_STREAMS];

// Until the protocol negotiates the stream count, there is exactly one.
int VideoStreamCount = 1;

static SOCKET rtpSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
#define RTP_RECV_PACKETS_BUFFERED 2048

// Initialize the video stream
void initializeVideoStream(void) {
    LC_ASSERT(VideoStreamCount >= 1 && VideoStreamCount <= MAX_VIDEO_STREAMS);

    for (int i = 0; i < VideoStreamCount; i++) {
        VideoStreams[i].streamIndex = i;
        VideoStreams[i].receivedFullFrame = false;
        initializeVideoDepacketizer(i, StreamConfig.packetSize);
        RtpvInitializeQueue(&VideoStreams[i].rtpQueue, i);
    }

    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
}

// True once any stream has assembled a complete frame.
static bool anyStreamReceivedFullFrame(void) {
    for (int i = 0; i < VideoStreamCount; i++) {
        if (VideoStreams[i].receivedFullFrame) {
            return true;
        }
    }
    return false;
}

// Whether the client submits decode units from our own thread rather than pulling them.
static bool usingDecoderThreads(void) {
    return (VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0;
}

// Clean up the video stream
void destroyVideoStream(void) {
    PltDestroyCryptoContext(decryptionCtx);

    for (int i = 0; i < VideoStreamCount; i++) {
        destroyVideoDepacketizer(i);
        RtpvCleanupQueue(&VideoStreams[i].rtpQueue);
    }
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;

    // TODO(multi-display): route by the stream index in the packet header once it exists
    PVIDEO_STREAM_CONTEXT stream = &VideoStreams[0];

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        encryptedBuffer = (char*)malloc(receiveSize);
        if (encryptedBuffer == NULL) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
    else {
        encryptedBuffer = NULL;
    }

    waitingForVideoMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        PRTP_PACKET packet;

        if (buffer == NULL) {
            buffer = (char*)malloc(bufferSize);
            if (buffer == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }

        err = recvUdpSocket(rtpSocket,
                            encrypted ? encryptedBuffer : buffer,
                            receiveSize,
                            useSelect);
        if (err < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if  (err == 0) {
            if (!receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }

            // Receive timed out; try again
            continue;
        }

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

            firstDataTimeMs = PltGetMillis();
        }

#ifndef LC_FUZZING
        if (!anyStreamReceivedFullFrame()) {
            if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                Limelog("Terminating connection due to lack of a successful video frame\n");
                ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                break;
            }
        }
#endif

        if (err < minSize) {
            // Runt packet
            continue;
        }

        // Decrypt the packet into the buffer if encryption is enabled
        if (encrypted) {
            PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)encryptedBuffer;

            // If this frame is below our current frame number, discard it before decryption
            // to save CPU cycles decrypting FEC shards for a frame we already reassembled.
            //
            // Since this is happening _before_ decryption, this packet is not trusted yet.
            // It's imperative that we do not mutate any state based on this packet until
            // after it has been decrypted successfully!
            //
            // It's possible for an attacker to inject a fake packet that has any value of
            // header fields they want, however this provides them no benefit because we will
            // simply drop said packet here (if it's below the current frame number) or it
            // will pass this check and be dropped during decryption (if contents is tampered)
            // or after decryption in the RTP queue (if it's a replay of a previous authentic
            // packet from the host).
            //
            // In short, an attacker spoofing this value via MITM or sending malicious values
            // impersonating the host from off-link doesn't gain them anything. If they have
            // a true MITM, they can DoS our connection by just dropping all our traffic, so
            // tampering with packets to fail this check doesn't accomplish anything they
            // couldn't already do. If they're not on-link, we just throw their malicious
            // traffic away (as mentioned in the paragraph above) and continue accepting
            // legitmate video traffic.
            if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&stream->rtpQueue)) {
                continue;
            }

            if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                   (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                   encHeader->iv, sizeof(encHeader->iv),
                                   encHeader->tag, sizeof(encHeader->tag),
                                   ((unsigned char*)(encHeader + 1)), err - sizeof(ENC_VIDEO_HEADER), // The ciphertext is after the header
                                   (unsigned char*)buffer, &err)) {
                Limelog("Failed to decrypt video packet!\n");
                continue;
            }
        }

        // Convert fields to host byte-order
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = BE16(packet->sequenceNumber);
        packet->timestamp = BE32(packet->timestamp);
        packet->ssrc = BE32(packet->ssrc);

        queueStatus = RtpvAddPacket(&stream->rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

        if (queueStatus == RTPF_RET_QUEUED) {
            // The queue owns the buffer
            buffer = NULL;
        }
    }

    if (buffer != NULL) {
        free(buffer);
    }

    if (encryptedBuffer != NULL) {
        free(encryptedBuffer);
    }
}

void notifyKeyFrameReceived(int streamIndex) {
    LC_ASSERT(streamIndex >= 0 && streamIndex < VideoStreamCount);

    // Remember that we got a full frame successfully
    VideoStreams[streamIndex].receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    PVIDEO_STREAM_CONTEXT stream = context;

    while (!PltIsThreadInterrupted(&stream->decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(stream->streamIndex, &frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    for (int i = 0; i < VideoStreamCount; i++) {
        VideoCallbacks.stop(i);

        // Wake up client code that may be waiting on the decode unit queue
        stopVideoDepacketizer(i);
    }

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if (usingDecoderThreads()) {
        for (int i = 0; i < VideoStreamCount; i++) {
            PltInterruptThread(&VideoStreams[i].decoderThread);
        }
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if (usingDecoderThreads()) {
        for (int i = 0; i < VideoStreamCount; i++) {
            PltJoinThread(&VideoStreams[i].decoderThread);
        }
    }

    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    for (int i = 0; i < VideoStreamCount; i++) {
        VideoCallbacks.cleanup(i);
    }
}

// Start the video stream
// Tear down the streams that were brought up before a failure partway through startup.
static void unwindStartedStreams(int startedCount, bool decoderThreadsStarted) {
    if (decoderThreadsStarted) {
        for (int i = 0; i < startedCount; i++) {
            PltInterruptThread(&VideoStreams[i].decoderThread);
        }
        for (int i = 0; i < startedCount; i++) {
            PltJoinThread(&VideoStreams[i].decoderThread);
        }
    }

    for (int i = 0; i < startedCount; i++) {
        VideoCallbacks.stop(i);
        VideoCallbacks.cleanup(i);
    }
}

int startVideoStream(void* rendererContext, int drFlags) {
    int err;
    int setupCount = 0;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    for (int i = 0; i < VideoStreamCount; i++) {
        err = VideoCallbacks.setup(i, NegotiatedVideoFormat, StreamConfig.width,
            StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
        if (err != 0) {
            for (int j = 0; j < setupCount; j++) {
                VideoCallbacks.cleanup(j);
            }
            return err;
        }
        setupCount++;
    }

    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              RTP_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
    if (rtpSocket == INVALID_SOCKET) {
        for (int i = 0; i < VideoStreamCount; i++) {
            VideoCallbacks.cleanup(i);
        }
        return LastSocketError();
    }

    for (int i = 0; i < VideoStreamCount; i++) {
        VideoCallbacks.start(i);
    }

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        unwindStartedStreams(VideoStreamCount, false);
        closeSocket(rtpSocket);
        return err;
    }

    if (usingDecoderThreads()) {
        for (int i = 0; i < VideoStreamCount; i++) {
            err = PltCreateThread("VideoDec", VideoDecoderThreadProc, &VideoStreams[i],
                                  &VideoStreams[i].decoderThread);
            if (err != 0) {
                PltInterruptThread(&receiveThread);
                PltJoinThread(&receiveThread);
                unwindStartedStreams(i, true);
                for (int j = i; j < VideoStreamCount; j++) {
                    VideoCallbacks.stop(j);
                    VideoCallbacks.cleanup(j);
                }
                closeSocket(rtpSocket);
                return err;
            }
        }
    }

    // Start pinging before reading the first frame so the host knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        for (int i = 0; i < VideoStreamCount; i++) {
            VideoCallbacks.stop(i);
            stopVideoDepacketizer(i);
        }
        PltInterruptThread(&receiveThread);
        if (usingDecoderThreads()) {
            for (int i = 0; i < VideoStreamCount; i++) {
                PltInterruptThread(&VideoStreams[i].decoderThread);
            }
        }
        PltJoinThread(&receiveThread);
        if (usingDecoderThreads()) {
            for (int i = 0; i < VideoStreamCount; i++) {
                PltJoinThread(&VideoStreams[i].decoderThread);
            }
        }
        closeSocket(rtpSocket);
        for (int i = 0; i < VideoStreamCount; i++) {
            VideoCallbacks.cleanup(i);
        }
        return err;
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(int streamIndex) {
    LC_ASSERT(streamIndex >= 0 && streamIndex < VideoStreamCount);
    return &VideoStreams[streamIndex].rtpQueue.stats;
}

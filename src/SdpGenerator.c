#include "Limelight-internal.h"
#include <inttypes.h>

#define MAX_OPTION_NAME_LEN 128

#define MAX_SDP_HEADER_LEN 128
#define MAX_SDP_TAIL_LEN 128

typedef struct _SDP_OPTION {
    char name[MAX_OPTION_NAME_LEN + 1];
    void* payload;
    int payloadLen;
    struct _SDP_OPTION* next;
} SDP_OPTION, *PSDP_OPTION;

// Cleanup the attribute list
static void freeAttributeList(PSDP_OPTION head) {
    PSDP_OPTION next;
    while (head != NULL) {
        next = head->next;
        free(head);
        head = next;
    }
}

// Get the size of the attribute list
static int getSerializedAttributeListSize(PSDP_OPTION head) {
    PSDP_OPTION currentEntry = head;
    size_t size = 0;
    while (currentEntry != NULL) {
        size += strlen("a=");
        size += strlen(currentEntry->name);
        size += strlen(":");
        size += currentEntry->payloadLen;
        size += strlen(" \r\n");

        currentEntry = currentEntry->next;
    }
    // Add one for the null terminator
    return (int)size + 1;
}

// Populate the serialized attribute list into a string
static int fillSerializedAttributeList(char* buffer, size_t length, PSDP_OPTION head) {
    PSDP_OPTION currentEntry = head;
    int offset = 0;
    while (currentEntry != NULL) {
        int ret = snprintf(&buffer[offset], length, "a=%s:", currentEntry->name);
        if (ret > 0 && (size_t)ret < length) {
            offset += ret;
            length -= ret;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        if ((size_t)currentEntry->payloadLen < length) {
            memcpy(&buffer[offset], currentEntry->payload, currentEntry->payloadLen);
            offset += currentEntry->payloadLen;
            length -= currentEntry->payloadLen;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        ret = snprintf(&buffer[offset], length, " \r\n");
        if (ret > 0 && (size_t)ret < length) {
            offset += ret;
            length -= ret;
        }
        else {
            LC_ASSERT(false);
            break;
        }

        currentEntry = currentEntry->next;
    }

    // We should have only space for the null terminator left over
    LC_ASSERT(length == 1);
    return offset;
}

// Add an attribute
static int addAttributeBinary(PSDP_OPTION* head, char* name, const void* payload, int payloadLen) {
    PSDP_OPTION option, currentOption;

    option = malloc(sizeof(*option) + payloadLen);
    if (option == NULL) {
        return -1;
    }

    if (!PltSafeStrcpy(option->name, sizeof(option->name), name)) {
        free(option);
        return -1;
    }

    option->next = NULL;
    option->payloadLen = payloadLen;
    option->payload = (void*)(option + 1);
    memcpy(option->payload, payload, payloadLen);

    if (*head == NULL) {
        *head = option;
    }
    else {
        currentOption = *head;
        while (currentOption->next != NULL) {
            currentOption = currentOption->next;
        }
        currentOption->next = option;
    }

    return 0;
}

// Add an attribute string
static int addAttributeString(PSDP_OPTION* head, char* name, const char* payload) {
    // We purposefully omit the null terminating character
    return addAttributeBinary(head, name, payload, (int)strlen(payload));
}

#define NVFF_BASE             0x07
#define NVFF_AUDIO_ENCRYPTION 0x20
#define NVFF_RI_ENCRYPTION    0x80

static int addStreamOptions(PSDP_OPTION* head) {
    int err = 0;
    char payloadStr[32];

    // This must be initialized to false already
    LC_ASSERT(!AudioEncryptionEnabled);

    unsigned int featureFlags;

    // RI encryption is always enabled
    featureFlags = NVFF_BASE | NVFF_RI_ENCRYPTION;

    // Enable audio encryption if the client opted in or the host required it
    if ((StreamConfig.encryptionFlags & ENCFLG_AUDIO) || (EncryptionFeaturesEnabled & SS_ENC_AUDIO)) {
        featureFlags |= NVFF_AUDIO_ENCRYPTION;
        AudioEncryptionEnabled = true;
    }

    snprintf(payloadStr, sizeof(payloadStr), "%u", featureFlags);
    err |= addAttributeString(head, "x-nv-general.featureFlags", payloadStr);

    // Ask for the encrypted control protocol to ensure remote input will be encrypted.
    // This used to be done via separate RI encryption, but now it is all or nothing.
    err |= addAttributeString(head, "x-nv-general.useReliableUdp", "13");

    // Require at least 2 FEC packets for small frames. If a frame has fewer data shards
    // than would generate 2 FEC shards, it will increase the FEC percentage for that frame
    // above the set value (even going as high as 200% FEC to generate 2 FEC shards from a
    // 1 data shard frame).
    err |= addAttributeString(head, "x-nv-vqos[0].fec.minRequiredFecPackets", "2");

    // BLL-FEC appears to adjust dynamically based on the loss rate and instantaneous bitrate
    // of each frame, however we can't dynamically control it from our side yet. As a result,
    // the effective FEC amount is significantly lower (single digit percentages for many
    // large frames) and the result is worse performance during packet loss. Disabling BLL-FEC
    // results in GFE 3.26 falling back to the legacy FEC method as we would like.
    err |= addAttributeString(head, "x-nv-vqos[0].bllFec.enable", "0");
    
    // Disable dynamic resolution switching
    err |= addAttributeString(head, "x-nv-vqos[0].drc.enable", "0");

    // Recovery mode can cause the FEC percentage to change mid-frame, which
    // breaks many assumptions in RTP FEC queue.
    err |= addAttributeString(head, "x-nv-general.enableRecoveryMode", "0");

    return err;
}

// Emit the attributes that describe one video stream. Stream 0's are written inline by
// getAttributesList() along with the session-wide ones; this fills in the rest so that a
// multi-display session describes every display it asked for.
static int addVideoStreamAttributes(PSDP_OPTION* head, int index, int adjustedBitrate) {
    char name[64];
    char payloadStr[92];
    unsigned char slicesPerFrame;
    int err = 0;

#define ADD_STREAM_ATTR(group, field, value) \
    snprintf(name, sizeof(name), "%s[%d].%s", (group), index, (field)); \
    err |= addAttributeString(head, name, (value))

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.videoStreams[index].width);
    ADD_STREAM_ATTR("x-nv-video", "clientViewportWd", payloadStr);
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.videoStreams[index].height);
    ADD_STREAM_ATTR("x-nv-video", "clientViewportHt", payloadStr);
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.videoStreams[index].fps);
    ADD_STREAM_ATTR("x-nv-video", "maxFPS", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", adjustedBitrate);
    ADD_STREAM_ATTR("x-nv-video", "initialBitrateKbps", payloadStr);
    ADD_STREAM_ATTR("x-nv-video", "initialPeakBitrateKbps", payloadStr);
    ADD_STREAM_ATTR("x-nv-vqos", "bw.minimumBitrateKbps", payloadStr);
    ADD_STREAM_ATTR("x-nv-vqos", "bw.maximumBitrateKbps", payloadStr);

    // Codec selection is a property of this client, so it matches stream 0.
    slicesPerFrame = (unsigned char)(VideoCallbacks.capabilities >> 24);
    if (slicesPerFrame == 0) {
        slicesPerFrame = 1;
    }
    snprintf(payloadStr, sizeof(payloadStr), "%d", slicesPerFrame);
    ADD_STREAM_ATTR("x-nv-video", "videoEncoderSlicesPerFrame", payloadStr);

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_AV1) {
        ADD_STREAM_ATTR("x-nv-vqos", "bitStreamFormat", "2");
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        ADD_STREAM_ATTR("x-nv-vqos", "bitStreamFormat", "1");
    }
    else {
        ADD_STREAM_ATTR("x-nv-vqos", "bitStreamFormat", "0");
    }

    ADD_STREAM_ATTR("x-nv-video", "dynamicRangeMode",
                    (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_10BIT) ? "1" : "0");
    ADD_STREAM_ATTR("x-ss-video", "chromaSamplingType",
                    (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_YUV444) ? "1" : "0");
    ADD_STREAM_ATTR("x-nv-video", "maxNumReferenceFrames",
                    isReferenceFrameInvalidationSupportedByDecoder() ? "0" : "1");

    snprintf(payloadStr, sizeof(payloadStr), "%d",
             (StreamConfig.colorSpace << 1) | StreamConfig.colorRange);
    ADD_STREAM_ATTR("x-nv-video", "encoderCscMode", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.clientRefreshRateX100);
    ADD_STREAM_ATTR("x-nv-video", "clientRefreshRateX100", payloadStr);

#undef ADD_STREAM_ATTR

    return err;
}

static PSDP_OPTION getAttributesList(char*urlSafeAddr) {
    PSDP_OPTION optionHead;
    char payloadStr[92];
    int audioChannelCount;
    int audioChannelMask;
    int err;
    int adjustedBitrate;

    // This must have been resolved to either local or remote by now
    LC_ASSERT(StreamConfig.streamingRemotely != STREAM_CFG_AUTO);

    optionHead = NULL;
    err = 0;

    // Send client feature flags to Sunshine hosts
    uint32_t moonlightFeatureFlags = ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1;
    snprintf(payloadStr, sizeof(payloadStr), "%" PRIu32, moonlightFeatureFlags);
    err |= addAttributeString(&optionHead, "x-ml-general.featureFlags", payloadStr);

    // New-style control stream encryption is low overhead, so we enable it any time it is supported
    if (EncryptionFeaturesSupported & SS_ENC_CONTROL_V2) {
        EncryptionFeaturesEnabled |= SS_ENC_CONTROL_V2;
    }

    // If video encryption is supported by the host and desired by the client, use it
    if ((EncryptionFeaturesSupported & SS_ENC_VIDEO) && (StreamConfig.encryptionFlags & ENCFLG_VIDEO)) {
        EncryptionFeaturesEnabled |= SS_ENC_VIDEO;
    }
    else if ((EncryptionFeaturesRequested & SS_ENC_VIDEO) && !(StreamConfig.encryptionFlags & ENCFLG_VIDEO)) {
        // If video encryption is explicitly requested by the host but *not* by the client,
        // we'll encrypt anyway (since we are capable of doing so) and print a warning.
        Limelog("Enabling video encryption by host request despite client opt-out. Performance may suffer!");
        EncryptionFeaturesEnabled |= SS_ENC_VIDEO;
    }

    // If audio encryption is supported by the host and desired by the client, use it
    if ((EncryptionFeaturesSupported & SS_ENC_AUDIO) && (StreamConfig.encryptionFlags & ENCFLG_AUDIO)) {
        EncryptionFeaturesEnabled |= SS_ENC_AUDIO;
    }
    else if ((EncryptionFeaturesRequested & SS_ENC_AUDIO) && !(StreamConfig.encryptionFlags & ENCFLG_AUDIO)) {
        // If audio encryption is explicitly requested by the host but *not* by the client,
        // we'll encrypt anyway (since we are capable of doing so) and print a warning.
        Limelog("Enabling audio encryption by host request despite client opt-out. Audio quality may suffer!");
        EncryptionFeaturesEnabled |= SS_ENC_AUDIO;
    }

    snprintf(payloadStr, sizeof(payloadStr), "%" PRIu32, EncryptionFeaturesEnabled);
    err |= addAttributeString(&optionHead, "x-ss-general.encryptionEnabled", payloadStr);

    // Enable YUV444 if requested
    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_YUV444) {
        err |= addAttributeString(&optionHead, "x-ss-video[0].chromaSamplingType", "1");
    }
    else {
        err |= addAttributeString(&optionHead, "x-ss-video[0].chromaSamplingType", "0");
    }

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.width);
    err |= addAttributeString(&optionHead, "x-nv-video[0].clientViewportWd", payloadStr);
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.height);
    err |= addAttributeString(&optionHead, "x-nv-video[0].clientViewportHt", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.fps);
    err |= addAttributeString(&optionHead, "x-nv-video[0].maxFPS", payloadStr);

    // Adjust the video packet size to account for encryption overhead
    if (EncryptionFeaturesEnabled & SS_ENC_VIDEO) {
        LC_ASSERT(StreamConfig.packetSize % 16 == 0);
        StreamConfig.packetSize -= sizeof(ENC_VIDEO_HEADER);
        LC_ASSERT(StreamConfig.packetSize % 16 == 0);
    }
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.packetSize);
    err |= addAttributeString(&optionHead, "x-nv-video[0].packetSize", payloadStr);

    err |= addAttributeString(&optionHead, "x-nv-video[0].rateControlMode", "4");

    err |= addAttributeString(&optionHead, "x-nv-video[0].timeoutLengthMs", "7000");
    err |= addAttributeString(&optionHead, "x-nv-video[0].framesWithInvalidRefThreshold", "0");

    // 20% of the video bitrate will added to the user-specified bitrate for FEC
    adjustedBitrate = (int)(StreamConfig.bitrate * 0.80);

    // Use more strict bitrate logic when streaming remotely. The theory here is that remote
    // streaming is much more bandwidth sensitive. Someone might select 5 Mbps because that's
    // really all they have, so we need to be careful not to exceed the cap, even counting
    // things like audio and control data.
    if (StreamConfig.streamingRemotely == STREAM_CFG_REMOTE) {
        // Subtract 500 Kbps to leave room for audio and control. On remote streams,
        // GFE will use 96Kbps stereo audio. For local streams, it will choose 512Kbps.
        if (adjustedBitrate > 500) {
            adjustedBitrate -= 500;
        }
    }

    // GFE currently imposes a limit of 100 Mbps for the video bitrate. It will automatically
    // impose that on maximumBitrateKbps but not on initialBitrateKbps. We will impose the cap
    // ourselves so initialBitrateKbps does not exceed maximumBitrateKbps.
    adjustedBitrate = adjustedBitrate > 100000 ? 100000 : adjustedBitrate;

    // We don't support dynamic bitrate scaling properly (it tends to bounce between min and max and never
    // settle on the optimal bitrate if it's somewhere in the middle), so we'll just latch the bitrate
    // to the requested value.
    snprintf(payloadStr, sizeof(payloadStr), "%d", adjustedBitrate);

    err |= addAttributeString(&optionHead, "x-nv-video[0].initialBitrateKbps", payloadStr);
    err |= addAttributeString(&optionHead, "x-nv-video[0].initialPeakBitrateKbps", payloadStr);

    err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.minimumBitrateKbps", payloadStr);
    err |= addAttributeString(&optionHead, "x-nv-vqos[0].bw.maximumBitrateKbps", payloadStr);

    // Send the configured bitrate to Sunshine hosts, so they can adjust for dynamic FEC percentage
    snprintf(payloadStr, sizeof(payloadStr), "%u", StreamConfig.bitrate);
    err |= addAttributeString(&optionHead, "x-ml-video.configuredBitrateKbps", payloadStr);
    
    // FEC must be enabled for proper packet sequencing to be done by RTP FEC queue
    err |= addAttributeString(&optionHead, "x-nv-vqos[0].fec.enable", "1");
    
    err |= addAttributeString(&optionHead, "x-nv-vqos[0].videoQualityScoreUpdateTime", "5000");

    // If the remote host is local (RFC 1918), enable QoS tagging for our traffic. Windows qWave
    // will disable it if the host is off-link, *however* Windows may get it wrong in cases where
    // the host is directly connected to the Internet without a NAT. In this case, it may send DSCP
    // marked traffic off-link and it could lead to black holes due to misconfigured ISP hardware
    // or CPE. For this reason, we only enable it in cases where it looks like it will work.
    //
    // Even though IPv6 hardware should be much less likely to have this issue, we can't tell
    // if our address is a NAT64 synthesized IPv6 address or true end-to-end IPv6. If it's the
    // former, it may have the same problem as other IPv4 traffic.
    if (StreamConfig.streamingRemotely == STREAM_CFG_LOCAL) {
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].qosTrafficType", "5");
        err |= addAttributeString(&optionHead, "x-nv-aqos.qosTrafficType", "4");
    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].qosTrafficType", "0");
        err |= addAttributeString(&optionHead, "x-nv-aqos.qosTrafficType", "0");
    }

    err |= addStreamOptions(&optionHead);

    audioChannelCount = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration);
    audioChannelMask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(StreamConfig.audioConfiguration);

    unsigned char slicesPerFrame;

    // Use slicing for increased performance on some decoders
    slicesPerFrame = (unsigned char)(VideoCallbacks.capabilities >> 24);
    if (slicesPerFrame == 0) {
        // If not using slicing, we request 1 slice per frame
        slicesPerFrame = 1;
    }
    snprintf(payloadStr, sizeof(payloadStr), "%d", slicesPerFrame);
    err |= addAttributeString(&optionHead, "x-nv-video[0].videoEncoderSlicesPerFrame", payloadStr);

    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_AV1) {
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "2");
    }
    else if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        err |= addAttributeString(&optionHead, "x-nv-clientSupportHevc", "1");
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "1");

    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-clientSupportHevc", "0");
        err |= addAttributeString(&optionHead, "x-nv-vqos[0].bitStreamFormat", "0");
    }

    // Enable HDR if requested
    if (NegotiatedVideoFormat & VIDEO_FORMAT_MASK_10BIT) {
        err |= addAttributeString(&optionHead, "x-nv-video[0].dynamicRangeMode", "1");
    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-video[0].dynamicRangeMode", "0");
    }

    // If the decoder supports reference frame invalidation, that indicates it also supports
    // the maximum number of reference frames allowed by the codec. Even if we can't use RFI
    // due to lack of host support, we can still allow the host to pick a number of reference
    // frames greater than 1 to improve encoding efficiency.
    if (isReferenceFrameInvalidationSupportedByDecoder()) {
        err |= addAttributeString(&optionHead, "x-nv-video[0].maxNumReferenceFrames", "0");
    }
    else {
        // Restrict the video stream to 1 reference frame if we're not using
        // reference frame invalidation. This helps to improve compatibility with
        // some decoders that don't like the default of having 16 reference frames.
        err |= addAttributeString(&optionHead, "x-nv-video[0].maxNumReferenceFrames", "1");
    }

    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.clientRefreshRateX100);
    err |= addAttributeString(&optionHead, "x-nv-video[0].clientRefreshRateX100", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", audioChannelCount);
    err |= addAttributeString(&optionHead, "x-nv-audio.surround.numChannels", payloadStr);
    snprintf(payloadStr, sizeof(payloadStr), "%d", audioChannelMask);
    err |= addAttributeString(&optionHead, "x-nv-audio.surround.channelMask", payloadStr);
    if (audioChannelCount > 2) {
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.enable", "1");
    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.enable", "0");
    }

    if (StreamConfig.bitrate >= HIGH_AUDIO_BITRATE_THRESHOLD && audioChannelCount > 2 &&
            HighQualitySurroundSupported && (AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0) {
        // Enable high quality mode for surround sound
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.AudioQuality", "1");

        // Let the audio stream code know that it needs to disable coupled streams when
        // decoding this audio stream.
        HighQualitySurroundEnabled = true;

        // Use 5 ms frames since we don't have a slow decoder
        AudioPacketDuration = 5;
    }
    else {
        err |= addAttributeString(&optionHead, "x-nv-audio.surround.AudioQuality", "0");
        HighQualitySurroundEnabled = false;

        if ((AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) ||
                 ((AudioCallbacks.capabilities & CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION) != 0 &&
                   StreamConfig.bitrate < LOW_AUDIO_BITRATE_TRESHOLD)) {
            // Use 10 ms packets for slow devices and networks to balance latency and bandwidth usage
            AudioPacketDuration = 10;
        }
        else {
            // Use 5 ms packets by default for lowest latency
            AudioPacketDuration = 5;
        }
    }

    snprintf(payloadStr, sizeof(payloadStr), "%d", AudioPacketDuration);
    err |= addAttributeString(&optionHead, "x-nv-aqos.packetDuration", payloadStr);

    snprintf(payloadStr, sizeof(payloadStr), "%d", (StreamConfig.colorSpace << 1) | StreamConfig.colorRange);
    err |= addAttributeString(&optionHead, "x-nv-video[0].encoderCscMode", payloadStr);

    // Ask for one stream per host display, and name the display each one captures.
    snprintf(payloadStr, sizeof(payloadStr), "%d", StreamConfig.videoStreamCount);
    err |= addAttributeString(&optionHead, "x-ss-general.videoStreamCount", payloadStr);

    for (int i = 0; i < StreamConfig.videoStreamCount; i++) {
        char name[64];

        snprintf(name, sizeof(name), "x-ss-video[%d].outputName", i);
        err |= addAttributeString(&optionHead, name, StreamConfig.videoStreams[i].hostDisplayName);

        // Stream 0's attributes were written inline above, along with the session-wide ones.
        if (i > 0) {
            err |= addVideoStreamAttributes(&optionHead, i, adjustedBitrate);
        }
    }

    if (err == 0) {
        return optionHead;
    }

    freeAttributeList(optionHead);
    return NULL;
}

// Populate the SDP header with required information
static int fillSdpHeader(char* buffer, size_t length, int rtspClientVersion, char*urlSafeAddr) {
    return snprintf(buffer, length,
        "v=0\r\n"
        "o=android 0 %d IN %s %s\r\n"
        "s=NVIDIA Streaming Client\r\n",
        rtspClientVersion,
        RemoteAddr.ss_family == AF_INET ? "IPv4" : "IPv6",
        urlSafeAddr);
}

// Populate the SDP tail with required information
static int fillSdpTail(char* buffer, size_t length) {
    LC_ASSERT(VideoPortNumber != 0);
    return snprintf(buffer, length,
        "t=0 0\r\n"
        "m=video %d  \r\n",
        VideoPortNumber);
}

// Get the SDP attributes for the stream config
char* getSdpPayloadForStreamConfig(int rtspClientVersion, int* length) {
    PSDP_OPTION attributeList;
    int attributeListSize;
    int offset, written;
    char* payload;
    char urlSafeAddr[URLSAFESTRING_LEN];

    addrToUrlSafeString(&RemoteAddr, urlSafeAddr, sizeof(urlSafeAddr));

    attributeList = getAttributesList(urlSafeAddr);
    if (attributeList == NULL) {
        return NULL;
    }

    attributeListSize = getSerializedAttributeListSize(attributeList);
    payload = malloc(MAX_SDP_HEADER_LEN + MAX_SDP_TAIL_LEN + attributeListSize);
    if (payload == NULL) {
        freeAttributeList(attributeList);
        return NULL;
    }

    offset = 0;
    written = fillSdpHeader(payload, MAX_SDP_HEADER_LEN, rtspClientVersion, urlSafeAddr);
    if (written < 0 || written >= MAX_SDP_HEADER_LEN) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }
    written = fillSerializedAttributeList(&payload[offset], attributeListSize, attributeList);
    if (written < 0 || written >= attributeListSize) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }
    written = fillSdpTail(&payload[offset], MAX_SDP_TAIL_LEN);
    if (written < 0 || written >= MAX_SDP_TAIL_LEN) {
        LC_ASSERT(false);
        free(payload);
        freeAttributeList(attributeList);
        return NULL;
    }
    else {
        offset += written;
    }

    freeAttributeList(attributeList);
    *length = offset;
    return payload;
}

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// VideoToolbox donanım H.264 kodlayıcı, düşük gecikmeli hız kontrolüyle.
/// Çıktıyı Annex B (00 00 00 01 + NAL) olarak verir; anahtar karelerin başına SPS/PPS eklenir.
final class H264Encoder {
    private var session: VTCompressionSession?
    private let onFrame: (Data, Bool) -> Void

    init(width: Int, height: Int, fps: Int, bitrate: Int, onFrame: @escaping (Data, Bool) -> Void) throws {
        self.onFrame = onFrame

        let lowLatency = kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String
        let requireHW = kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String
        let specs: [[String: Any]] = [
            [lowLatency: true, requireHW: true],
            [lowLatency: true],
            [:],
        ]

        var created: VTCompressionSession?
        var status: OSStatus = -1
        for spec in specs {
            created = nil
            status = VTCompressionSessionCreate(
                allocator: kCFAllocatorDefault,
                width: Int32(width),
                height: Int32(height),
                codecType: kCMVideoCodecType_H264,
                encoderSpecification: spec as CFDictionary,
                imageBufferAttributes: nil,
                compressedDataAllocator: nil,
                outputCallback: nil,
                refcon: nil,
                compressionSessionOut: &created)
            if status == noErr, created != nil { break }
        }
        guard status == noErr, let s = created else {
            throw YansiError("VideoToolbox kodlayıcı oluşturulamadı (OSStatus \(status))")
        }
        session = s

        func set(_ key: CFString, _ value: CFTypeRef) {
            let st = VTSessionSetProperty(s, key: key, value: value)
            if st != noErr { fputs("uyarı: \(key) ayarlanamadı (\(st))\n", stderr) }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: fps * 10))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 10))
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        VTCompressionSessionPrepareToEncodeFrames(s)
    }

    deinit {
        if let s = session {
            VTCompressionSessionInvalidate(s)
        }
    }

    func encode(_ pixelBuffer: CVPixelBuffer, forceKey: Bool) {
        guard let s = session else { return }
        let pts = CMTime(seconds: ProcessInfo.processInfo.systemUptime, preferredTimescale: 1_000_000)
        let props: CFDictionary? =
            forceKey ? ([kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary) : nil
        VTCompressionSessionEncodeFrame(
            s, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, _, sampleBuffer in
            guard status == noErr, let sampleBuffer, let self else { return }
            self.emit(sampleBuffer)
        }
    }

    private func emit(_ sb: CMSampleBuffer) {
        var isKey = true
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[String: Any]],
            let first = arr.first
        {
            isKey = !((first[kCMSampleAttachmentKey_NotSync as String] as? Bool) ?? false)
        }
        guard let fmt = CMSampleBufferGetFormatDescription(sb),
            let block = CMSampleBufferGetDataBuffer(sb)
        else { return }

        let startCode: [UInt8] = [0, 0, 0, 1]
        let total = CMBlockBufferGetDataLength(block)
        var out = Data()
        out.reserveCapacity(total + 256)

        var nalHeaderLength: Int32 = 4
        var paramCount = 0
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &paramCount, nalUnitHeaderLengthOut: &nalHeaderLength)

        if isKey {
            for i in 0..<paramCount {
                var ptr: UnsafePointer<UInt8>?
                var size = 0
                let st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    fmt, parameterSetIndex: i, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                if st == noErr, let ptr {
                    out.append(contentsOf: startCode)
                    out.append(ptr, count: size)
                }
            }
        }

        var raw = [UInt8](repeating: 0, count: total)
        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: total, destination: &raw) == noErr else {
            return
        }

        // AVCC (uzunluk önekli) → Annex B
        let hl = Int(nalHeaderLength)
        var off = 0
        while off + hl <= total {
            var nalLen = 0
            for k in 0..<hl { nalLen = (nalLen << 8) | Int(raw[off + k]) }
            off += hl
            guard nalLen > 0, off + nalLen <= total else { break }
            out.append(contentsOf: startCode)
            out.append(contentsOf: raw[off..<(off + nalLen)])
            off += nalLen
        }

        onFrame(out, isKey)
    }
}

// LZFSEStreamTests.swift

#if canImport(Compression)

import Compression
import Foundation
import lzfse
import Testing

@Suite
struct LZFSEStreamTests {

    /// Data packed by Apple's `compression_stream` (LZFSE) decodes byte for byte through
    /// `lzfse_stream`, fed in small src chunks into a small dst - the streaming worst case.
    @Test(arguments: [0, 1, 1_000, 300_000, 6_000_000])
    func decodesAppleStreamInSmallChunks(size: Int) throws {
        let original = Self.sampleData(size: size)
        let compressed = try Self.appleEncode(original)

        let decoded = try Self.streamDecode(compressed, srcChunk: 4_096, dstChunk: 1_000)

        #expect(decoded == original)
    }

    /// Same stream fed whole into one large dst, to cover the path with no back pressure.
    @Test
    func decodesAppleStreamInOneGo() throws {
        let original = Self.sampleData(size: 3_000_000)
        let compressed = try Self.appleEncode(original)

        let decoded = try Self.streamDecode(compressed, srcChunk: compressed.count, dstChunk: 8_000_000)

        #expect(decoded == original)
    }

    /// A stream cut short fails on finalize instead of returning partial data as success.
    @Test
    func truncatedStreamFails() throws {
        let original = Self.sampleData(size: 500_000)
        let compressed = try Self.appleEncode(original)
        let truncated = compressed.prefix(compressed.count / 2)

        #expect(throws: StreamError.self) {
            _ = try Self.streamDecode(Data(truncated), srcChunk: 4_096, dstChunk: 1_000)
        }
    }

    /// Real archive from the CDN, path in `LZFSE_REAL_FILE`; compared with Apple's own decode.
    @Test(.enabled(if: ProcessInfo.processInfo.environment["LZFSE_REAL_FILE"] != nil))
    func decodesRealArchive() throws {
        let path = ProcessInfo.processInfo.environment["LZFSE_REAL_FILE"]!
        let compressed = try Data(contentsOf: URL(fileURLWithPath: path))

        let expected = try Self.appleDecode(compressed)
        let decoded = try Self.streamDecode(compressed, srcChunk: 64 * 1_024, dstChunk: 2 * 1_024 * 1_024)

        #expect(decoded.count == expected.count)
        #expect(decoded == expected)
    }

    // MARK: Private

    private enum StreamError: Error {
        case failed(String)
    }

    /// Compressible data with matches near the 256 KB history limit.
    private static func sampleData(size: Int) -> Data {
        var generator = SystemRandomNumberGenerator()
        var bytes = [UInt8]()
        bytes.reserveCapacity(size)
        while bytes.count < size {
            let distance = [7, 300, 65_000, 200_000, 262_000].randomElement(using: &generator)!
            let length = Int.random(in: 1...600, using: &generator)
            if distance <= bytes.count, Bool.random(using: &generator) {
                let start = bytes.count - distance
                for index in 0..<length where bytes.count < size {
                    bytes.append(bytes[start + index])
                }
            } else {
                for _ in 0..<length where bytes.count < size {
                    bytes.append(UInt8.random(in: 0...15, using: &generator))
                }
            }
        }
        return Data(bytes)
    }

    private static func appleEncode(_ data: Data) throws -> Data {
        try appleProcess(data, operation: COMPRESSION_STREAM_ENCODE)
    }

    private static func appleDecode(_ data: Data) throws -> Data {
        try appleProcess(data, operation: COMPRESSION_STREAM_DECODE)
    }

    private static func appleProcess(_ data: Data, operation: compression_stream_operation) throws -> Data {
        let streamPointer = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer {
            streamPointer.deallocate()
        }
        var stream = streamPointer.pointee
        let initStatus = compression_stream_init(&stream, operation, COMPRESSION_LZFSE)
        guard initStatus == COMPRESSION_STATUS_OK else {
            throw StreamError.failed("compression_stream_init")
        }
        defer {
            compression_stream_destroy(&stream)
        }

        let bufferSize = 1 << 20
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        defer {
            buffer.deallocate()
        }

        var output = Data()
        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let source = raw.bindMemory(to: UInt8.self)
            stream.src_ptr = source.baseAddress ?? UnsafePointer(buffer)
            stream.src_size = source.count
            while true {
                stream.dst_ptr = buffer
                stream.dst_size = bufferSize
                let status = compression_stream_process(&stream, Int32(COMPRESSION_STREAM_FINALIZE.rawValue))
                output.append(buffer, count: bufferSize - stream.dst_size)
                switch status {
                case COMPRESSION_STATUS_OK:
                    continue
                case COMPRESSION_STATUS_END:
                    return
                default:
                    throw StreamError.failed("compression_stream_process")
                }
            }
        }
        return output
    }

    private static func streamDecode(_ data: Data, srcChunk: Int, dstChunk: Int) throws -> Data {
        var stream = lzfse_stream()
        guard lzfse_stream_init(&stream) == LZFSE_STREAM_STATUS_OK else {
            throw StreamError.failed("lzfse_stream_init")
        }
        defer {
            lzfse_stream_destroy(&stream)
        }

        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: dstChunk)
        defer {
            buffer.deallocate()
        }

        var output = Data()
        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let source = raw.bindMemory(to: UInt8.self)
            var offset = 0
            while true {
                let chunk = min(srcChunk, source.count - offset)
                let isLast = offset + chunk == source.count
                stream.src_ptr = source.baseAddress.map { UnsafePointer($0 + offset) }
                stream.src_size = chunk
                offset += chunk

                while true {
                    stream.dst_ptr = buffer
                    stream.dst_size = dstChunk
                    let status = lzfse_stream_process(&stream, isLast ? 1 : 0)
                    output.append(buffer, count: dstChunk - stream.dst_size)
                    switch status {
                    case LZFSE_STREAM_STATUS_END:
                        return
                    case LZFSE_STREAM_STATUS_OK:
                        // Same chunk again while it still produces or consumes.
                        if stream.dst_size == 0 || stream.src_size > 0 {
                            continue
                        }
                    default:
                        throw StreamError.failed("lzfse_stream_process")
                    }
                    break
                }

                guard isLast == false else {
                    throw StreamError.failed("Input ended before the end of stream")
                }
            }
        }
        return output
    }
}

#endif

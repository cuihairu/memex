// 纯逻辑库导出面（ArkTS 壳统一从 '@memex/core' 引入；Node 测试直接
// 引各模块源文件，不经本文件——本文件仅 hvigor 编译用）。
export { MsgType, NoticeUrgency, ProtocolViolation, msgTypeName, encodeEnvelope, decodeEnvelope, textEnvelope, ackEnvelope, loginEnvelope, logoutEnvelope, pingEnvelope } from './wire'
export { LENGTH_PREFIX_SIZE, MAX_FRAME_SIZE, encodeFrame, DecodeStatus, FrameDecoder } from './frame_codec'
export { DEFAULT_PORT, PORT_MAX, ServerAddress } from './address'
export { NoticeGrade, fromProtoNumber, composeNoticeText } from './format'
export { InMemoryChatStore } from './chat_store'
export { InitStore, Route, RouteGuard, InitEntry, InitGate } from './route'
export { Transport, TransportError } from './transport'
export { WireChannel, classifyFailure } from './wire_channel'
export { MemexClient } from './client'
export { ChatSession, ChatSessionListener, ConnectOutcome } from './session'
export {
  FilesApiError, FilesClient, DEFAULT_FILES_PORT,
  utf8ToLatin1, restoreUtf8Name, sanitizeFileName, dedupeFileName,
} from './files_client'
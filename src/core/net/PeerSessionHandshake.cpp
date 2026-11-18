#include "core/net/PeerSession.h"

#include "core/crypto/Crypto.h"
#include "core/crypto/DeviceIdentity.h"
#include "core/platform/EventLog.h"
#include "core/protocol/HandshakeProtocol.h"
#include "core/crypto/SessionCipher.h"

#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <stdexcept>

namespace bm {

namespace {

constexpr int kConnectTimeoutMs = 10000;

// 握手帧序号固定为零，密文帧从 1 开始；载荷长度此时就是明文长度
Bytes BuildPlainFrame(MessageType type, std::span<const uint8_t> payload) {
  Bytes bytes(kHeaderSize + payload.size(), 0);
  WriteFrameHeader(std::span<uint8_t>(bytes.data(), kHeaderSize), type, static_cast<uint32_t>(payload.size()), 0);
  if (!payload.empty()) std::memcpy(bytes.data() + kHeaderSize, payload.data(), payload.size());
  return bytes;
}

// 阻塞 connect 在系统默认超时前不会返回，局域网拨号必须自己限 10 秒
SOCKET ConnectSocket(const std::string& host, int port, const std::atomic<bool>* canceled) {
  const auto isCanceled = [canceled] { return canceled && canceled->load(); };
  if (isCanceled()) throw std::runtime_error("连接已取消");
  ADDRINFOEXW hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const auto node = ToWide(host);
  const auto service = std::to_wstring(port);
  std::unique_ptr<void, decltype(&CloseHandle)> resolved(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
  if (!resolved) throw std::runtime_error("无法创建地址解析事件");
  OVERLAPPED lookup{};
  lookup.hEvent = resolved.get();
  HANDLE lookupHandle = nullptr;
  PADDRINFOEXW result = nullptr;
  const uint64_t deadline = TickMs() + kConnectTimeoutMs;
  int resolveError = GetAddrInfoExW(node.c_str(), service.c_str(), NS_DNS, nullptr, &hints, &result, nullptr,
                                    &lookup, nullptr, &lookupHandle);
  if (resolveError == WSA_IO_PENDING) {
    // 取消时等待解析器归还 OVERLAPPED 和结果存储，再销毁事件。
    DWORD state;
    do {
      state = WaitForSingleObject(resolved.get(), 50);
    } while (state == WAIT_TIMEOUT && !isCanceled() && TickMs() < deadline);
    if (state != WAIT_OBJECT_0) {
      GetAddrInfoExCancel(&lookupHandle);
      WaitForSingleObject(resolved.get(), INFINITE);
    }
    resolveError = GetAddrInfoExOverlappedResult(&lookup);
  }
  const std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)> addresses(result, &FreeAddrInfoExW);
  if (isCanceled()) throw std::runtime_error("连接已取消");
  if (TickMs() >= deadline) throw std::runtime_error("解析或连接对端超过10秒");
  if (resolveError != 0) throw std::runtime_error("对端地址无法解析：" + host);
  SOCKET socket = INVALID_SOCKET;
  int lastError = 0;
  for (const ADDRINFOEXW* entry = addresses.get(); entry && !isCanceled() && TickMs() < deadline;
       entry = entry->ai_next) {
    socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (socket == INVALID_SOCKET) continue;
    u_long nonBlocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonBlocking) != 0) {
      lastError = WSAGetLastError();
      closesocket(socket);
      socket = INVALID_SOCKET;
      continue;
    }
    bool connected = connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
      WSAPOLLFD descriptor{};
      descriptor.fd = socket;
      descriptor.events = POLLWRNORM;
      int ready = 0;
      while (ready == 0 && TickMs() < deadline && !isCanceled())
        ready = WSAPoll(&descriptor, 1, 50);
      if (ready == 0 && !isCanceled()) {
        closesocket(socket);
        throw std::runtime_error("解析或连接对端超过10秒");
      }
      if (ready > 0) {
        int error = 0;
        int length = sizeof(error);
        connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 &&
                    error == 0;
      }
    }
    if (connected) {
      nonBlocking = 0;
      if (ioctlsocket(socket, FIONBIO, &nonBlocking) == 0) break;
    }
    lastError = WSAGetLastError();
    closesocket(socket);
    socket = INVALID_SOCKET;
  }
  if (isCanceled()) {
    if (socket != INVALID_SOCKET) closesocket(socket);
    throw std::runtime_error("连接已取消");
  }
  if (socket == INVALID_SOCKET && TickMs() >= deadline) throw std::runtime_error("解析或连接对端超过10秒");
  if (socket == INVALID_SOCKET)
    throw std::runtime_error("无法连接对端：" + host + "，错误码 " + std::to_string(lastError));
  return socket;
}

}  // namespace

PeerSessionPtr PeerSession::Connect(const std::string& host, int port, const HandshakeOptions& options,
                                    const std::atomic<bool>* canceled) {
  InitializeNetworking();
  if (port < 1 || port > 65535) throw std::out_of_range("对端端口非法");
  const SOCKET socket = ConnectSocket(host, port, canceled);
  return Complete(static_cast<uintptr_t>(socket), true, options, canceled);
}

PeerSessionPtr PeerSession::Accept(uintptr_t socket, const HandshakeOptions& options,
                                   const std::atomic<bool>* canceled) {
  return Complete(socket, false, options, canceled);
}

PeerSessionPtr PeerSession::Complete(uintptr_t socket, bool initiator, const HandshakeOptions& options,
                                     const std::atomic<bool>* canceled) {
  PeerSessionPtr session;
  try {
    if (options.localGeometry == nullptr || options.identity == nullptr)
      throw std::invalid_argument("握手缺少本机显示布局或设备身份");
    // make_shared 的构造发生在控制块分配之后，构造失败仍由本函数关闭句柄。
    struct ConcreteSession : PeerSession {
      ConcreteSession(uintptr_t handle, const DesktopGeometry& geometry, bool client)
          : PeerSession(handle, geometry, client) {}
    };
    session = std::make_shared<ConcreteSession>(socket, *options.localGeometry, initiator);
    session->canceled_ = canceled;
    // 主机在这一步登记会话，关闭时才能等待握手收尾
    if (options.constructed) options.constructed(session);
    session->ThrowIfCanceled();
    session->Authenticate(options);
    session->ThrowIfCanceled();
  } catch (...) {
    if (session) {
      session->FinishHandshake();
      session->Close();
    } else {
      const SOCKET handle = static_cast<SOCKET>(socket);
      shutdown(handle, SD_BOTH);
      closesocket(handle);
    }
    throw;
  }
  // 握手结束后密码对象不再被握手线程使用，收尾线程可以安全销毁它
  session->canceled_ = nullptr;
  session->FinishHandshake();
  return session;
}

void PeerSession::FinishHandshake() {
  {
    std::lock_guard lock(closeMutex_);
    handshakeDone_ = true;
  }
  closeSignal_.notify_all();
}

void PeerSession::ThrowIfCanceled() const {
  if (canceled_ != nullptr && canceled_->load()) throw std::runtime_error("连接已取消");
  if (IsClosed()) throw std::runtime_error("会话已被关闭");
}

PeerSession::WireFrame PeerSession::ReadWireFrame() {
  WireFrame frame;
  frame.header.resize(kHeaderSize);
  ReadExact(frame.header);
  if (!TryReadFrameHeader(frame.header, frame.info)) throw std::runtime_error("帧头非法或协议不匹配");
  frame.payload.resize(frame.info.payloadLength);
  if (!frame.payload.empty()) ReadExact(frame.payload);
  return frame;
}

PeerSession::WireFrame PeerSession::ReadPlain(MessageType expectedType) {
  WireFrame frame = ReadWireFrame();
  if (frame.info.type != expectedType || frame.info.sequence != 0)
    throw std::runtime_error("握手消息顺序、角色或帧序号非法");
  return frame;
}

Bytes PeerSession::ReadEncrypted(MessageType expectedType) {
  WireFrame frame = ReadWireFrame();
  Bytes payload = cipher_->Open(frame.header, frame.payload);
  if (frame.info.type != expectedType) throw std::runtime_error("加密握手消息顺序非法");
  return payload;
}

bool PeerSession::ReadEncryptedBoolean(MessageType expectedType) {
  const Bytes payload = ReadEncrypted(expectedType);
  if (payload.size() != 1 || payload[0] > 1) throw std::runtime_error("配对状态载荷非法");
  return payload[0] == 1;
}

void PeerSession::Authenticate(const HandshakeOptions& options) {
  std::unique_ptr<crypto::EcdhKey> ephemeral = crypto::EcdhKey::Generate();
  const Bytes helloPayload =
      HandshakeProtocol::EncodeHello(options.localName, localGeometry_, options.localPort, *options.identity, *ephemeral);
  const Bytes localHello =
      BuildPlainFrame(initiator_ ? MessageType::ClientHello : MessageType::ServerHello, helloPayload);
  WireFrame peerHello;
  HelloData peer;
  if (initiator_) {
    WriteFrame(localHello);
    peerHello = ReadPlain(MessageType::ServerHello);
    peer = HandshakeProtocol::ParseHello(peerHello.payload);
  } else {
    peerHello = ReadPlain(MessageType::ClientHello);
    peer = HandshakeProtocol::ParseHello(peerHello.payload);
    WriteFrame(localHello);
  }
  // 签名覆盖两个原始 Hello 帧，顺序按角色固定
  const Bytes peerHelloBytes = peerHello.All();
  const std::span<const uint8_t> clientFrame =
      initiator_ ? std::span<const uint8_t>(localHello) : std::span<const uint8_t>(peerHelloBytes);
  const std::span<const uint8_t> serverFrame =
      initiator_ ? std::span<const uint8_t>(peerHelloBytes) : std::span<const uint8_t>(localHello);
  const Bytes transcript = HandshakeProtocol::Transcript(clientFrame, serverFrame);

  if (initiator_) {
    const Bytes proof = options.identity->Sign(HandshakeProtocol::ProofHash(transcript, true));
    WriteFrame(BuildPlainFrame(MessageType::IdentityProof, proof));
    HandshakeProtocol::VerifyProof(peer.identityKey, ReadPlain(MessageType::IdentityProof).payload, transcript, false);
  } else {
    HandshakeProtocol::VerifyProof(peer.identityKey, ReadPlain(MessageType::IdentityProof).payload, transcript, true);
    const Bytes proof = options.identity->Sign(HandshakeProtocol::ProofHash(transcript, false));
    WriteFrame(BuildPlainFrame(MessageType::IdentityProof, proof));
  }

  // 名称和地址不参与信任判断：先证明私钥持有，再核对用户选中的固定设备身份
  peerId_ = ToHex(crypto::Sha256(peer.identityKey));
  peerPublicKey_ = ToBase64(peer.identityKey);
  peerName_ = peer.name;
  peerGeometry_ = peer.desktop;
  peerPort_ = peer.port;
  if (peerId_ == options.identity->Id()) throw std::runtime_error("不能与使用同一持久身份的设备建立会话");
  if (options.expectedPeerId != nullptr && !options.expectedPeerId->empty() && peerId_ != *options.expectedPeerId)
    throw std::runtime_error("对端身份与所选设备不一致，已拒绝连接，未发起重新配对");
  ThrowIfCanceled();
  // 未认证连接只占短期限；确认私钥身份后才给用户核对配对码的时间
  SetReceiveTimeout(kHandshakeTimeoutMs);
  handshakeTransaction_ = ToHex(crypto::Sha256(transcript));
  {
    Bytes secret = ephemeral->DeriveSecret(peer.ephemeralKey);
    try {
      cipher_ = std::make_unique<SessionCipher>(secret, transcript, initiator_);
    } catch (...) {
      SecureZero(secret);
      throw;
    }
    SecureZero(secret);
  }

  ElectPhysicalConnection(options);
  const bool localTrust = options.isTrusted && options.isTrusted(peerId_, peerPublicKey_);
  const uint8_t localTrustByte = localTrust ? 1 : 0;
  WriteEncrypted(MessageType::TrustState, std::span<const uint8_t>(&localTrustByte, 1));
  const bool remoteTrust = ReadEncryptedBoolean(MessageType::TrustState);
  ExchangePairing(localTrust, remoteTrust, options);

  // 密钥确认绑定角色、完整转录与双方信任/接受状态，双方成功后才交给主控保存
  Bytes context = transcript;
  context.push_back(static_cast<uint8_t>((initiator_ ? localTrust : remoteTrust) ? 1 : 0));
  context.push_back(static_cast<uint8_t>((initiator_ ? remoteTrust : localTrust) ? 1 : 0));
  context.push_back(1);
  context.push_back(1);
  WriteEncrypted(MessageType::KeyConfirmation, cipher_->Confirmation(initiator_, context));
  const Bytes confirmation = ReadEncrypted(MessageType::KeyConfirmation);
  const Bytes expected = cipher_->Confirmation(!initiator_, context);
  if (!FixedTimeEquals(confirmation, expected)) throw std::runtime_error("对端密钥确认失败");
  ThrowIfCanceled();
}

void PeerSession::ElectPhysicalConnection(const HandshakeOptions& options) {
  const Bytes transaction = FromHex(handshakeTransaction_);
  // 身份较小的一端唯一负责选举；另一端只认可这条连接上的认证转录，双方弹窗必属同一事务
  if (options.identity->Id() < peerId_) {
    if (!options.selectCandidate || !options.selectCandidate(*this)) throw std::runtime_error("同一设备已有选定连接");
    WriteEncrypted(MessageType::TrustState, transaction);
    if (!FixedTimeEquals(ReadEncrypted(MessageType::TrustState), transaction))
      throw std::runtime_error("握手事务确认失败");
  } else {
    if (!FixedTimeEquals(ReadEncrypted(MessageType::TrustState), transaction))
      throw std::runtime_error("握手事务选择非法");
    if (!options.selectCandidate || !options.selectCandidate(*this)) throw std::runtime_error("同一设备已有选定连接");
    WriteEncrypted(MessageType::TrustState, transaction);
  }
}

void PeerSession::ExchangePairing(bool localTrust, bool remoteTrust, const HandshakeOptions& options) {
  std::atomic<bool> aborted{false};
  std::atomic<int> remote{0};  // 1 接受 / -1 拒绝 / -2 读取失败
  std::exception_ptr remoteError;
  // 远端决定与本机确认并行：对端拒绝或断线时本机不必等对话框
  std::thread reader([&] {
    try {
      const bool accepted = ReadEncryptedBoolean(MessageType::PairingDecision);
      remote.store(accepted ? 1 : -1);
      if (!accepted) aborted.store(true);
    } catch (...) {
      remoteError = std::current_exception();
      aborted.store(true);  // 连接已断：让本机对话框也能立刻收起来
      remote.store(-2);
    }
  });
  bool accepted = true;
  try {
    if (!(localTrust && remoteTrust)) {
      const PairingRequest request{peerId_,  peerName_, peerPublicKey_, cipher_->ShortAuthenticationCode(),
                                   peerId_, handshakeTransaction_};
      accepted = options.confirmPairing ? options.confirmPairing(request, aborted) : false;
    }
    // 决定发送与加入读取线程都受同一异常路径保护。
    const uint8_t decision = accepted ? 1 : 0;
    WriteEncrypted(MessageType::PairingDecision, std::span<const uint8_t>(&decision, 1));
  } catch (...) {
    shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
    reader.join();
    throw;
  }

  reader.join();
  if (remoteError) std::rethrow_exception(remoteError);
  if (!accepted) throw std::runtime_error("本机拒绝了配对");
  if (remote.load() != 1) throw std::runtime_error("对方拒绝了配对");
}

}  // namespace bm

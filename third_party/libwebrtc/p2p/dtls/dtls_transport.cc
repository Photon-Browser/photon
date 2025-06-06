/*
 *  Copyright 2011 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "p2p/dtls/dtls_transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/crypto/crypto_options.h"
#include "api/dtls_transport_interface.h"
#include "api/rtc_error.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/scoped_refptr.h"
#include "api/sequence_checker.h"
#include "api/transport/stun.h"
#include "api/units/timestamp.h"
#include "logging/rtc_event_log/events/rtc_event_dtls_transport_state.h"
#include "logging/rtc_event_log/events/rtc_event_dtls_writable_state.h"
#include "p2p/base/ice_transport_internal.h"
#include "p2p/base/packet_transport_internal.h"
#include "p2p/dtls/dtls_stun_piggyback_controller.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "p2p/dtls/dtls_utils.h"
#include "rtc_base/buffer.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/network/ecn_marking.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network_route.h"
#include "rtc_base/rtc_certificate.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/ssl_stream_adapter.h"
#include "rtc_base/stream.h"
#include "rtc_base/time_utils.h"

namespace cricket {

// We don't pull the RTP constants from rtputils.h, to avoid a layer violation.
static const size_t kMinRtpPacketLen = 12;

// Maximum number of pending packets in the queue. Packets are read immediately
// after they have been written, so a capacity of "1" is sufficient.
//
// However, this bug seems to indicate that's not the case: crbug.com/1063834
// So, temporarily increasing it to 2 to see if that makes a difference.
static const size_t kMaxPendingPackets = 2;

// Minimum and maximum values for the initial DTLS handshake timeout. We'll pick
// an initial timeout based on ICE RTT estimates, but clamp it to this range.
static const int kMinHandshakeTimeoutMs = 50;
static const int kMaxHandshakeTimeoutMs = 3000;
// This effectively disables the handshake timeout.
static const int kDisabledHandshakeTimeoutMs = 3600 * 1000 * 24;

static bool IsRtpPacket(rtc::ArrayView<const uint8_t> payload) {
  const uint8_t* u = payload.data();
  return (payload.size() >= kMinRtpPacketLen && (u[0] & 0xC0) == 0x80);
}

StreamInterfaceChannel::StreamInterfaceChannel(
    IceTransportInternal* ice_transport)
    : ice_transport_(ice_transport),
      state_(rtc::SS_OPEN),
      packets_(kMaxPendingPackets, kMaxDtlsPacketLen) {}

void StreamInterfaceChannel::SetDtlsStunPiggybackController(
    DtlsStunPiggybackController* dtls_stun_piggyback_controller) {
  dtls_stun_piggyback_controller_ = dtls_stun_piggyback_controller;
}

rtc::StreamResult StreamInterfaceChannel::Read(rtc::ArrayView<uint8_t> buffer,
                                               size_t& read,
                                               int& /* error */) {
  RTC_DCHECK_RUN_ON(&callback_sequence_);

  if (state_ == rtc::SS_CLOSED)
    return rtc::SR_EOS;
  if (state_ == rtc::SS_OPENING)
    return rtc::SR_BLOCK;

  if (!packets_.ReadFront(buffer.data(), buffer.size(), &read)) {
    return rtc::SR_BLOCK;
  }

  return rtc::SR_SUCCESS;
}

rtc::StreamResult StreamInterfaceChannel::Write(
    rtc::ArrayView<const uint8_t> data,
    size_t& written,
    int& /* error */) {
  RTC_DCHECK_RUN_ON(&callback_sequence_);

  // If we try to use DTLS-in-STUN, DTLS packets will be sent as part of STUN
  // packets and are consumed here.
  if (ice_transport_->config().dtls_handshake_in_stun &&
      dtls_stun_piggyback_controller_ &&
      dtls_stun_piggyback_controller_->MaybeConsumePacket(data)) {
    written = data.size();
    return rtc::SR_SUCCESS;
  }

  // Always succeeds, since this is an unreliable transport anyway.
  // TODO(zhihuang): Should this block if ice_transport_'s temporarily
  // unwritable?
  rtc::PacketOptions packet_options;
  ice_transport_->SendPacket(reinterpret_cast<const char*>(data.data()),
                             data.size(), packet_options);
  written = data.size();
  return rtc::SR_SUCCESS;
}

bool StreamInterfaceChannel::OnPacketReceived(const char* data, size_t size) {
  RTC_DCHECK_RUN_ON(&callback_sequence_);
  if (packets_.size() > 0) {
    RTC_LOG(LS_WARNING) << "Packet already in queue.";
  }
  bool ret = packets_.WriteBack(data, size, NULL);
  if (!ret) {
    // Somehow we received another packet before the SSLStreamAdapter read the
    // previous one out of our temporary buffer. In this case, we'll log an
    // error and still signal the read event, hoping that it will read the
    // packet currently in packets_.
    RTC_LOG(LS_ERROR) << "Failed to write packet to queue.";
  }
  FireEvent(rtc::SE_READ, 0);
  return ret;
}

rtc::StreamState StreamInterfaceChannel::GetState() const {
  RTC_DCHECK_RUN_ON(&callback_sequence_);
  return state_;
}

void StreamInterfaceChannel::Close() {
  RTC_DCHECK_RUN_ON(&callback_sequence_);
  packets_.Clear();
  state_ = rtc::SS_CLOSED;
}

DtlsTransport::DtlsTransport(IceTransportInternal* ice_transport,
                             const webrtc::CryptoOptions& crypto_options,
                             webrtc::RtcEventLog* event_log,
                             rtc::SSLProtocolVersion max_version)
    : component_(ice_transport->component()),
      ice_transport_(ice_transport),
      downward_(nullptr),
      srtp_ciphers_(crypto_options.GetSupportedDtlsSrtpCryptoSuites()),
      ssl_max_version_(max_version),
      event_log_(event_log),
      dtls_stun_piggyback_controller_(
          [this](rtc::ArrayView<const uint8_t> piggybacked_dtls_packet) {
            if (piggybacked_dtls_callback_ == nullptr) {
              return;
            }
            piggybacked_dtls_callback_(
                this, rtc::ReceivedPacket(piggybacked_dtls_packet,
                                          rtc::SocketAddress()));
          }) {
  RTC_DCHECK(ice_transport_);
  ConnectToIceTransport();
}

DtlsTransport::~DtlsTransport() {
  if (ice_transport_) {
    ice_transport_->SetDtlsPiggybackingCallbacks(nullptr, nullptr, nullptr);
    ice_transport_->DeregisterReceivedPacketCallback(this);
  }
}

webrtc::DtlsTransportState DtlsTransport::dtls_state() const {
  return dtls_state_;
}

const std::string& DtlsTransport::transport_name() const {
  return ice_transport_->transport_name();
}

int DtlsTransport::component() const {
  return component_;
}

bool DtlsTransport::IsDtlsActive() const {
  return dtls_active_;
}

bool DtlsTransport::SetLocalCertificate(
    const rtc::scoped_refptr<rtc::RTCCertificate>& certificate) {
  if (dtls_active_) {
    if (certificate == local_certificate_) {
      // This may happen during renegotiation.
      RTC_LOG(LS_INFO) << ToString() << ": Ignoring identical DTLS identity";
      return true;
    } else {
      RTC_LOG(LS_ERROR) << ToString()
                        << ": Can't change DTLS local identity in this state";
      return false;
    }
  }

  if (certificate) {
    local_certificate_ = certificate;
    dtls_active_ = true;
  } else {
    RTC_LOG(LS_INFO) << ToString()
                     << ": NULL DTLS identity supplied. Not doing DTLS";
  }

  return true;
}

rtc::scoped_refptr<rtc::RTCCertificate> DtlsTransport::GetLocalCertificate()
    const {
  return local_certificate_;
}

bool DtlsTransport::SetDtlsRole(rtc::SSLRole role) {
  if (dtls_) {
    RTC_DCHECK(dtls_role_);
    if (*dtls_role_ != role) {
      RTC_LOG(LS_ERROR)
          << "SSL Role can't be reversed after the session is setup.";
      return false;
    }
    return true;
  }

  dtls_role_ = role;
  return true;
}

bool DtlsTransport::GetDtlsRole(rtc::SSLRole* role) const {
  if (!dtls_role_) {
    return false;
  }
  *role = *dtls_role_;
  return true;
}

bool DtlsTransport::GetSslCipherSuite(int* cipher) const {
  if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
    return false;
  }

  return dtls_->GetSslCipherSuite(cipher);
}

std::optional<absl::string_view> DtlsTransport::GetTlsCipherSuiteName() const {
  if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
    return std::nullopt;
  }
  return dtls_->GetTlsCipherSuiteName();
}

webrtc::RTCError DtlsTransport::SetRemoteParameters(
    absl::string_view digest_alg,
    const uint8_t* digest,
    size_t digest_len,
    std::optional<rtc::SSLRole> role) {
  rtc::Buffer remote_fingerprint_value(digest, digest_len);
  bool is_dtls_restart =
      dtls_active_ && remote_fingerprint_value_ != remote_fingerprint_value;
  // Set SSL role. Role must be set before fingerprint is applied, which
  // initiates DTLS setup.
  if (role) {
    if (is_dtls_restart) {
      dtls_role_ = *role;
    } else {
      if (!SetDtlsRole(*role)) {
        return webrtc::RTCError(webrtc::RTCErrorType::INVALID_PARAMETER,
                                "Failed to set SSL role for the transport.");
      }
    }
  }
  // Apply remote fingerprint.
  if (!SetRemoteFingerprint(digest_alg, digest, digest_len)) {
    return webrtc::RTCError(webrtc::RTCErrorType::INVALID_PARAMETER,
                            "Failed to apply remote fingerprint.");
  }
  return webrtc::RTCError::OK();
}

bool DtlsTransport::SetRemoteFingerprint(absl::string_view digest_alg,
                                         const uint8_t* digest,
                                         size_t digest_len) {
  rtc::Buffer remote_fingerprint_value(digest, digest_len);

  // Once we have the local certificate, the same remote fingerprint can be set
  // multiple times.
  if (dtls_active_ && remote_fingerprint_value_ == remote_fingerprint_value &&
      !digest_alg.empty()) {
    // This may happen during renegotiation.
    RTC_LOG(LS_INFO) << ToString()
                     << ": Ignoring identical remote DTLS fingerprint";
    return true;
  }

  // If the other side doesn't support DTLS, turn off `dtls_active_`.
  // TODO(deadbeef): Remove this. It's dangerous, because it relies on higher
  // level code to ensure DTLS is actually used, but there are tests that
  // depend on it, for the case where an m= section is rejected. In that case
  // SetRemoteFingerprint shouldn't even be called though.
  if (digest_alg.empty()) {
    RTC_DCHECK(!digest_len);
    RTC_LOG(LS_INFO) << ToString() << ": Other side didn't support DTLS.";
    dtls_active_ = false;
    return true;
  }

  // Otherwise, we must have a local certificate before setting remote
  // fingerprint.
  if (!dtls_active_) {
    RTC_LOG(LS_ERROR) << ToString()
                      << ": Can't set DTLS remote settings in this state.";
    return false;
  }

  // At this point we know we are doing DTLS
  bool fingerprint_changing = remote_fingerprint_value_.size() > 0u;
  remote_fingerprint_value_ = std::move(remote_fingerprint_value);
  remote_fingerprint_algorithm_ = std::string(digest_alg);

  if (dtls_ && !fingerprint_changing) {
    // This can occur if DTLS is set up before a remote fingerprint is
    // received. For instance, if we set up DTLS due to receiving an early
    // ClientHello.
    rtc::SSLPeerCertificateDigestError err = dtls_->SetPeerCertificateDigest(
        remote_fingerprint_algorithm_, remote_fingerprint_value_);
    if (err != rtc::SSLPeerCertificateDigestError::NONE) {
      RTC_LOG(LS_ERROR) << ToString()
                        << ": Couldn't set DTLS certificate digest.";
      set_dtls_state(webrtc::DtlsTransportState::kFailed);
      // If the error is "verification failed", don't return false, because
      // this means the fingerprint was formatted correctly but didn't match
      // the certificate from the DTLS handshake. Thus the DTLS state should go
      // to "failed", but SetRemoteDescription shouldn't fail.
      return err == rtc::SSLPeerCertificateDigestError::VERIFICATION_FAILED;
    }
    return true;
  }

  // If the fingerprint is changing, we'll tear down the DTLS association and
  // create a new one, resetting our state.
  if (dtls_ && fingerprint_changing) {
    dtls_.reset(nullptr);
    set_dtls_state(webrtc::DtlsTransportState::kNew);
    set_writable(false);
  }

  if (!SetupDtls()) {
    set_dtls_state(webrtc::DtlsTransportState::kFailed);
    return false;
  }

  return true;
}

std::unique_ptr<rtc::SSLCertChain> DtlsTransport::GetRemoteSSLCertChain()
    const {
  if (!dtls_) {
    return nullptr;
  }

  return dtls_->GetPeerSSLCertChain();
}

bool DtlsTransport::ExportSrtpKeyingMaterial(
    rtc::ZeroOnFreeBuffer<uint8_t>& keying_material) {
  return dtls_ ? dtls_->ExportSrtpKeyingMaterial(keying_material) : false;
}

bool DtlsTransport::SetupDtls() {
  RTC_DCHECK(dtls_role_);
  {
    auto downward = std::make_unique<StreamInterfaceChannel>(ice_transport_);
    StreamInterfaceChannel* downward_ptr = downward.get();

    downward_ptr->SetDtlsStunPiggybackController(
        &dtls_stun_piggyback_controller_);
    dtls_ = rtc::SSLStreamAdapter::Create(
        std::move(downward),
        [this](rtc::SSLHandshakeError error) { OnDtlsHandshakeError(error); },
        ice_transport_->field_trials());
    if (!dtls_) {
      RTC_LOG(LS_ERROR) << ToString() << ": Failed to create DTLS adapter.";
      return false;
    }
    downward_ = downward_ptr;
  }

  dtls_->SetIdentity(local_certificate_->identity()->Clone());
  dtls_->SetMaxProtocolVersion(ssl_max_version_);
  dtls_->SetServerRole(*dtls_role_);
  dtls_->SetEventCallback(
      [this](int events, int err) { OnDtlsEvent(events, err); });
  if (remote_fingerprint_value_.size() &&
      dtls_->SetPeerCertificateDigest(remote_fingerprint_algorithm_,
                                      remote_fingerprint_value_) !=
          rtc::SSLPeerCertificateDigestError::NONE) {
    RTC_LOG(LS_ERROR) << ToString()
                      << ": Couldn't set DTLS certificate digest.";
    return false;
  }

  // Set up DTLS-SRTP, if it's been enabled.
  if (!srtp_ciphers_.empty()) {
    if (!dtls_->SetDtlsSrtpCryptoSuites(srtp_ciphers_)) {
      RTC_LOG(LS_ERROR) << ToString() << ": Couldn't set DTLS-SRTP ciphers.";
      return false;
    }
  } else {
    RTC_LOG(LS_INFO) << ToString() << ": Not using DTLS-SRTP.";
  }

  RTC_LOG(LS_INFO) << ToString() << ": DTLS setup complete.";

  // If the underlying ice_transport is already writable at this point, we may
  // be able to start DTLS right away.
  MaybeStartDtls();
  return true;
}

bool DtlsTransport::GetSrtpCryptoSuite(int* cipher) const {
  if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
    return false;
  }

  return dtls_->GetDtlsSrtpCryptoSuite(cipher);
}

bool DtlsTransport::GetSslVersionBytes(int* version) const {
  if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
    return false;
  }

  return dtls_->GetSslVersionBytes(version);
}

uint16_t DtlsTransport::GetSslPeerSignatureAlgorithm() const {
  if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
    return rtc::kSslSignatureAlgorithmUnknown;  // "not applicable"
  }
  return dtls_->GetPeerSignatureAlgorithm();
}

// Called from upper layers to send a media packet.
int DtlsTransport::SendPacket(const char* data,
                              size_t size,
                              const rtc::PacketOptions& options,
                              int flags) {
  if (!dtls_active_) {
    // Not doing DTLS.
    return ice_transport_->SendPacket(data, size, options);
  }

  switch (dtls_state()) {
    case webrtc::DtlsTransportState::kNew:
      // Can't send data until the connection is active.
      // TODO(ekr@rtfm.com): assert here if dtls_ is NULL?
      return -1;
    case webrtc::DtlsTransportState::kConnecting:
      // Can't send data until the connection is active.
      return -1;
    case webrtc::DtlsTransportState::kConnected:
      if (flags & PF_SRTP_BYPASS) {
        RTC_DCHECK(!srtp_ciphers_.empty());
        if (!IsRtpPacket(rtc::MakeArrayView(
                reinterpret_cast<const uint8_t*>(data), size))) {
          return -1;
        }

        return ice_transport_->SendPacket(data, size, options);
      } else {
        size_t written;
        int error;
        return (dtls_->WriteAll(
                    rtc::MakeArrayView(reinterpret_cast<const uint8_t*>(data),
                                       size),
                    written, error) == rtc::SR_SUCCESS)
                   ? static_cast<int>(size)
                   : -1;
      }
    case webrtc::DtlsTransportState::kFailed:
      // Can't send anything when we're failed.
      RTC_LOG(LS_ERROR) << ToString()
                        << ": Couldn't send packet due to "
                           "webrtc::DtlsTransportState::kFailed.";
      return -1;
    case webrtc::DtlsTransportState::kClosed:
      // Can't send anything when we're closed.
      RTC_LOG(LS_ERROR) << ToString()
                        << ": Couldn't send packet due to "
                           "webrtc::DtlsTransportState::kClosed.";
      return -1;
    default:
      RTC_DCHECK_NOTREACHED();
      return -1;
  }
}

IceTransportInternal* DtlsTransport::ice_transport() {
  return ice_transport_;
}

bool DtlsTransport::IsDtlsConnected() {
  return dtls_ && dtls_->IsTlsConnected();
}

bool DtlsTransport::receiving() const {
  return receiving_;
}

bool DtlsTransport::writable() const {
  return writable_;
}

int DtlsTransport::GetError() {
  return ice_transport_->GetError();
}

std::optional<rtc::NetworkRoute> DtlsTransport::network_route() const {
  return ice_transport_->network_route();
}

bool DtlsTransport::GetOption(rtc::Socket::Option opt, int* value) {
  return ice_transport_->GetOption(opt, value);
}

int DtlsTransport::SetOption(rtc::Socket::Option opt, int value) {
  return ice_transport_->SetOption(opt, value);
}

void DtlsTransport::ConnectToIceTransport() {
  RTC_DCHECK(ice_transport_);
  ice_transport_->SignalWritableState.connect(this,
                                              &DtlsTransport::OnWritableState);
  ice_transport_->RegisterReceivedPacketCallback(
      this, [&](rtc::PacketTransportInternal* transport,
                const rtc::ReceivedPacket& packet) {
        OnReadPacket(transport, packet);
      });

  ice_transport_->SignalSentPacket.connect(this, &DtlsTransport::OnSentPacket);
  ice_transport_->SignalReadyToSend.connect(this,
                                            &DtlsTransport::OnReadyToSend);
  ice_transport_->SignalReceivingState.connect(
      this, &DtlsTransport::OnReceivingState);
  ice_transport_->SignalNetworkRouteChanged.connect(
      this, &DtlsTransport::OnNetworkRouteChanged);
  ice_transport_->SetDtlsPiggybackingCallbacks(
      [this](StunMessageType stun_message_type) {
        return dtls_stun_piggyback_controller_.GetDataToPiggyback(
            stun_message_type);
      },
      [this](StunMessageType stun_message_type) {
        return dtls_stun_piggyback_controller_.GetAckToPiggyback(
            stun_message_type);
      },
      [this](const StunByteStringAttribute* data,
             const StunByteStringAttribute* ack) {
        dtls_stun_piggyback_controller_.ReportDataPiggybacked(data, ack);
      });

  SetPiggybackDtlsDataCallback([this](rtc::PacketTransportInternal* transport,
                                      const rtc::ReceivedPacket& packet) {
    RTC_DCHECK(dtls_active_);
    RTC_DCHECK(IsDtlsPacket(packet.payload()));
    if (!dtls_active_) {
      // Not doing DTLS.
      return;
    }
    if (!IsDtlsPacket(packet.payload())) {
      return;
    }
    OnReadPacket(ice_transport_, packet);
  });
}

// The state transition logic here is as follows:
// (1) If we're not doing DTLS-SRTP, then the state is just the
//     state of the underlying impl()
// (2) If we're doing DTLS-SRTP:
//     - Prior to the DTLS handshake, the state is neither receiving nor
//       writable
//     - When the impl goes writable for the first time we
//       start the DTLS handshake
//     - Once the DTLS handshake completes, the state is that of the
//       impl again
void DtlsTransport::OnWritableState(rtc::PacketTransportInternal* transport) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(transport == ice_transport_);
  RTC_LOG(LS_VERBOSE) << ToString()
                      << ": ice_transport writable state changed to "
                      << ice_transport_->writable();

  if (!dtls_active_) {
    // Not doing DTLS.
    // Note: SignalWritableState fired by set_writable.
    set_writable(ice_transport_->writable());
    return;
  }

  // The opportunistic attempt to do DTLS piggybacking failed.
  // Recreate the DTLS session. Note: this assumes we can consider
  // the previous DTLS session state beyond repair and no packet
  // reached the peer.
  if (ice_transport_->config().dtls_handshake_in_stun && dtls_ &&
      !was_ever_connected_ && !IsDtlsPiggybackSupportedByPeer() &&
      (dtls_state() == webrtc::DtlsTransportState::kConnecting ||
       dtls_state() == webrtc::DtlsTransportState::kNew)) {
    RTC_LOG(LS_INFO) << "DTLS piggybacking not supported, restarting...";
    ice_transport_->SetDtlsPiggybackingCallbacks(nullptr, nullptr, nullptr);

    downward_->SetDtlsStunPiggybackController(nullptr);
    dtls_.reset(nullptr);
    set_dtls_state(webrtc::DtlsTransportState::kNew);
    set_writable(false);

    if (!SetupDtls()) {
      RTC_LOG(LS_ERROR)
          << "Failed to setup DTLS again after attempted piggybacking.";
      set_dtls_state(webrtc::DtlsTransportState::kFailed);
      return;
    }
    // SetupDtls has called MaybeStartDtls() already.
    return;
  }

  switch (dtls_state()) {
    case webrtc::DtlsTransportState::kNew:
      MaybeStartDtls();
      break;
    case webrtc::DtlsTransportState::kConnected:
      was_ever_connected_ = true;
      // Note: SignalWritableState fired by set_writable.
      set_writable(ice_transport_->writable());
      break;
    case webrtc::DtlsTransportState::kConnecting:
      // Do nothing.
      break;
    case webrtc::DtlsTransportState::kFailed:
      // Should not happen. Do nothing.
      RTC_LOG(LS_ERROR) << ToString()
                        << ": OnWritableState() called in state "
                           "webrtc::DtlsTransportState::kFailed.";
      break;
    case webrtc::DtlsTransportState::kClosed:
      // Should not happen. Do nothing.
      RTC_LOG(LS_ERROR) << ToString()
                        << ": OnWritableState() called in state "
                           "webrtc::DtlsTransportState::kClosed.";
      break;
    case webrtc::DtlsTransportState::kNumValues:
      RTC_DCHECK_NOTREACHED();
      break;
  }
}

void DtlsTransport::OnReceivingState(rtc::PacketTransportInternal* transport) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(transport == ice_transport_);
  RTC_LOG(LS_VERBOSE) << ToString()
                      << ": ice_transport "
                         "receiving state changed to "
                      << ice_transport_->receiving();
  if (!dtls_active_ || dtls_state() == webrtc::DtlsTransportState::kConnected) {
    // Note: SignalReceivingState fired by set_receiving.
    set_receiving(ice_transport_->receiving());
  }
}

void DtlsTransport::OnReadPacket(rtc::PacketTransportInternal* transport,
                                 const rtc::ReceivedPacket& packet) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(transport == ice_transport_);

  if (!dtls_active_) {
    // Not doing DTLS.
    NotifyPacketReceived(packet);
    return;
  }

  switch (dtls_state()) {
    case webrtc::DtlsTransportState::kNew:
      if (dtls_) {
        RTC_LOG(LS_INFO) << ToString()
                         << ": Packet received before DTLS started.";
      } else {
        RTC_LOG(LS_WARNING) << ToString()
                            << ": Packet received before we know if we are "
                               "doing DTLS or not.";
      }
      // Cache a client hello packet received before DTLS has actually started.
      if (IsDtlsClientHelloPacket(packet.payload())) {
        RTC_LOG(LS_INFO) << ToString()
                         << ": Caching DTLS ClientHello packet until DTLS is "
                            "started.";
        cached_client_hello_.SetData(packet.payload());
        // If we haven't started setting up DTLS yet (because we don't have a
        // remote fingerprint/role), we can use the client hello as a clue that
        // the peer has chosen the client role, and proceed with the handshake.
        // The fingerprint will be verified when it's set.
        if (!dtls_ && local_certificate_) {
          SetDtlsRole(rtc::SSL_SERVER);
          SetupDtls();
        }
      } else {
        RTC_LOG(LS_INFO) << ToString()
                         << ": Not a DTLS ClientHello packet; dropping.";
      }
      break;

    case webrtc::DtlsTransportState::kConnecting:
    case webrtc::DtlsTransportState::kConnected:
      // We should only get DTLS or SRTP packets; STUN's already been demuxed.
      // Is this potentially a DTLS packet?
      if (IsDtlsPacket(packet.payload())) {
        if (!HandleDtlsPacket(packet.payload())) {
          RTC_LOG(LS_ERROR) << ToString() << ": Failed to handle DTLS packet.";
          return;
        }
      } else {
        // Not a DTLS packet; our handshake should be complete by now.
        if (dtls_state() != webrtc::DtlsTransportState::kConnected) {
          RTC_LOG(LS_ERROR) << ToString()
                            << ": Received non-DTLS packet before DTLS "
                               "complete.";
          return;
        }

        // And it had better be a SRTP packet.
        if (!IsRtpPacket(packet.payload())) {
          RTC_LOG(LS_ERROR)
              << ToString() << ": Received unexpected non-DTLS packet.";
          return;
        }

        // Sanity check.
        RTC_DCHECK(!srtp_ciphers_.empty());

        // Signal this upwards as a bypass packet.
        NotifyPacketReceived(
            packet.CopyAndSet(rtc::ReceivedPacket::kSrtpEncrypted));
      }
      break;
    case webrtc::DtlsTransportState::kFailed:
    case webrtc::DtlsTransportState::kClosed:
    case webrtc::DtlsTransportState::kNumValues:
      // This shouldn't be happening. Drop the packet.
      break;
  }
}

void DtlsTransport::OnSentPacket(rtc::PacketTransportInternal* /* transport */,
                                 const rtc::SentPacket& sent_packet) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  SignalSentPacket(this, sent_packet);
}

void DtlsTransport::OnReadyToSend(
    rtc::PacketTransportInternal* /* transport */) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  if (writable()) {
    SignalReadyToSend(this);
  }
}

void DtlsTransport::OnDtlsEvent(int sig, int err) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(dtls_);

  if (sig & rtc::SE_OPEN) {
    // This is the first time.
    RTC_LOG(LS_INFO) << ToString() << ": DTLS handshake complete.";
    // The check for OPEN shouldn't be necessary but let's make
    // sure we don't accidentally frob the state if it's closed.
    if (dtls_->GetState() == rtc::SS_OPEN) {
      int ssl_version_bytes;
      bool ret = dtls_->GetSslVersionBytes(&ssl_version_bytes);
      RTC_DCHECK(ret);
      dtls_stun_piggyback_controller_.SetDtlsHandshakeComplete(
          dtls_role_ == rtc::SSL_CLIENT,
          ssl_version_bytes == rtc::kDtls13VersionBytes);
      downward_->SetDtlsStunPiggybackController(nullptr);
      set_dtls_state(webrtc::DtlsTransportState::kConnected);
      set_writable(true);
    }
  }
  if (sig & rtc::SE_READ) {
    uint8_t buf[kMaxDtlsPacketLen];
    size_t read;
    int read_error;
    rtc::StreamResult ret;
    // The underlying DTLS stream may have received multiple DTLS records in
    // one packet, so read all of them.
    do {
      ret = dtls_->Read(buf, read, read_error);
      if (ret == rtc::SR_SUCCESS) {
        // TODO(bugs.webrtc.org/15368): It should be possible to use information
        // from the original packet here to populate socket address and
        // timestamp.
        NotifyPacketReceived(rtc::ReceivedPacket(
            rtc::MakeArrayView(buf, read), rtc::SocketAddress(),
            webrtc::Timestamp::Micros(rtc::TimeMicros()),
            rtc::EcnMarking::kNotEct, rtc::ReceivedPacket::kDtlsDecrypted));
      } else if (ret == rtc::SR_EOS) {
        // Remote peer shut down the association with no error.
        RTC_LOG(LS_INFO) << ToString() << ": DTLS transport closed by remote";
        set_writable(false);
        set_dtls_state(webrtc::DtlsTransportState::kClosed);
        NotifyOnClose();
      } else if (ret == rtc::SR_ERROR) {
        // Remote peer shut down the association with an error.
        RTC_LOG(LS_INFO)
            << ToString()
            << ": Closed by remote with DTLS transport error, code="
            << read_error;
        set_writable(false);
        set_dtls_state(webrtc::DtlsTransportState::kFailed);
        NotifyOnClose();
      }
    } while (ret == rtc::SR_SUCCESS);
  }
  if (sig & rtc::SE_CLOSE) {
    RTC_DCHECK(sig == rtc::SE_CLOSE);  // SE_CLOSE should be by itself.
    set_writable(false);
    if (!err) {
      RTC_LOG(LS_INFO) << ToString() << ": DTLS transport closed";
      set_dtls_state(webrtc::DtlsTransportState::kClosed);
    } else {
      RTC_LOG(LS_INFO) << ToString() << ": DTLS transport error, code=" << err;
      set_dtls_state(webrtc::DtlsTransportState::kFailed);
    }
  }
}

void DtlsTransport::OnNetworkRouteChanged(
    std::optional<rtc::NetworkRoute> network_route) {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  SignalNetworkRouteChanged(network_route);
}

void DtlsTransport::MaybeStartDtls() {
  RTC_DCHECK(ice_transport_);
  //  When adding the DTLS handshake in STUN we want to call StartSSL even
  //  before the ICE transport is ready.
  bool start_early_for_dtls_in_stun =
      ice_transport_->config().dtls_handshake_in_stun;
  if (dtls_ && (ice_transport_->writable() || start_early_for_dtls_in_stun)) {
    ConfigureHandshakeTimeout(start_early_for_dtls_in_stun);

    if (dtls_->StartSSL()) {
      // This should never fail:
      // Because we are operating in a nonblocking mode and all
      // incoming packets come in via OnReadPacket(), which rejects
      // packets in this state, the incoming queue must be empty. We
      // ignore write errors, thus any errors must be because of
      // configuration and therefore are our fault.
      RTC_DCHECK_NOTREACHED() << "StartSSL failed.";
      RTC_LOG(LS_ERROR) << ToString() << ": Couldn't start DTLS handshake";
      set_dtls_state(webrtc::DtlsTransportState::kFailed);
      return;
    }
    RTC_LOG(LS_INFO) << ToString()
                     << ": DtlsTransport: Started DTLS handshake active="
                     << IsDtlsActive();
    set_dtls_state(webrtc::DtlsTransportState::kConnecting);
    // Now that the handshake has started, we can process a cached ClientHello
    // (if one exists).
    if (cached_client_hello_.size()) {
      if (*dtls_role_ == rtc::SSL_SERVER) {
        RTC_LOG(LS_INFO) << ToString()
                         << ": Handling cached DTLS ClientHello packet.";
        if (!HandleDtlsPacket(cached_client_hello_)) {
          RTC_LOG(LS_ERROR) << ToString() << ": Failed to handle DTLS packet.";
        }
      } else {
        RTC_LOG(LS_WARNING) << ToString()
                            << ": Discarding cached DTLS ClientHello packet "
                               "because we don't have the server role.";
      }
      cached_client_hello_.Clear();
    }
  }
}

// Called from OnReadPacket when a DTLS packet is received.
bool DtlsTransport::HandleDtlsPacket(rtc::ArrayView<const uint8_t> payload) {
  // Pass to the StreamInterfaceChannel which ends up being passed to the DTLS
  // stack.
  return downward_->OnPacketReceived(
      reinterpret_cast<const char*>(payload.data()), payload.size());
}

void DtlsTransport::set_receiving(bool receiving) {
  if (receiving_ == receiving) {
    return;
  }
  receiving_ = receiving;
  SignalReceivingState(this);
}

void DtlsTransport::set_writable(bool writable) {
  if (writable_ == writable) {
    return;
  }
  if (event_log_) {
    event_log_->Log(
        std::make_unique<webrtc::RtcEventDtlsWritableState>(writable));
  }
  RTC_LOG(LS_VERBOSE) << ToString() << ": set_writable to: " << writable;
  writable_ = writable;
  if (writable_) {
    SignalReadyToSend(this);
  }
  SignalWritableState(this);
}

void DtlsTransport::set_dtls_state(webrtc::DtlsTransportState state) {
  if (dtls_state_ == state) {
    return;
  }
  if (event_log_) {
    event_log_->Log(
        std::make_unique<webrtc::RtcEventDtlsTransportState>(state));
  }
  RTC_LOG(LS_VERBOSE) << ToString() << ": set_dtls_state from:"
                      << static_cast<int>(dtls_state_) << " to "
                      << static_cast<int>(state);
  dtls_state_ = state;
  SendDtlsState(this, state);
}

void DtlsTransport::OnDtlsHandshakeError(rtc::SSLHandshakeError error) {
  SendDtlsHandshakeError(error);
}

void DtlsTransport::ConfigureHandshakeTimeout(bool uses_dtls_in_stun) {
  RTC_DCHECK(dtls_);
  std::optional<int> rtt_ms = ice_transport_->GetRttEstimate();
  if (uses_dtls_in_stun) {
    // Configure a very high timeout to effectively disable the DTLS timeout
    // and avoid fragmented resends. This is ok since DTLS-in-STUN caches
    // the handshake pacets and resends them using the pacing of ICE.
    RTC_LOG(LS_INFO) << ToString() << ": configuring DTLS handshake timeout "
                     << kDisabledHandshakeTimeoutMs << "ms for DTLS-in-STUN";
    dtls_->SetInitialRetransmissionTimeout(kDisabledHandshakeTimeoutMs);
  } else if (rtt_ms) {
    // Limit the timeout to a reasonable range in case the ICE RTT takes
    // extreme values.
    int initial_timeout_ms =
        std::max(kMinHandshakeTimeoutMs,
                 std::min(kMaxHandshakeTimeoutMs, 2 * (*rtt_ms)));
    RTC_LOG(LS_INFO) << ToString() << ": configuring DTLS handshake timeout "
                     << initial_timeout_ms << "ms based on ICE RTT " << *rtt_ms;

    dtls_->SetInitialRetransmissionTimeout(initial_timeout_ms);
  } else {
    RTC_LOG(LS_INFO)
        << ToString()
        << ": no RTT estimate - using default DTLS handshake timeout";
  }
}

void DtlsTransport::SetPiggybackDtlsDataCallback(
    absl::AnyInvocable<void(rtc::PacketTransportInternal* transport,
                            const rtc::ReceivedPacket& packet)> callback) {
  RTC_DCHECK(callback == nullptr || !piggybacked_dtls_callback_);
  piggybacked_dtls_callback_ = std::move(callback);
}

bool DtlsTransport::IsDtlsPiggybackSupportedByPeer() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(ice_transport_);
  return ice_transport_->config().dtls_handshake_in_stun &&
         dtls_stun_piggyback_controller_.state() !=
             DtlsStunPiggybackController::State::OFF;
}

bool DtlsTransport::IsDtlsPiggybackHandshaking() {
  RTC_DCHECK_RUN_ON(&thread_checker_);
  RTC_DCHECK(ice_transport_);
  return ice_transport_->config().dtls_handshake_in_stun &&
         (dtls_stun_piggyback_controller_.state() ==
              DtlsStunPiggybackController::State::TENTATIVE ||
          dtls_stun_piggyback_controller_.state() ==
              DtlsStunPiggybackController::State::CONFIRMED);
}

}  // namespace cricket

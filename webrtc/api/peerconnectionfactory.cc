/*
 *  Copyright 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/api/peerconnectionfactory.h"

#include <utility>

#include "webrtc/api/audiotrack.h"
#include "webrtc/api/localaudiosource.h"
#include "webrtc/api/mediaconstraintsinterface.h"
#include "webrtc/api/mediastream.h"
#include "webrtc/api/mediastreamproxy.h"
#include "webrtc/api/mediastreamtrackproxy.h"
#include "webrtc/api/peerconnection.h"
#include "webrtc/api/peerconnectionfactoryproxy.h"
#include "webrtc/api/peerconnectionproxy.h"
#include "webrtc/api/videosource.h"
#include "webrtc/api/videosourceproxy.h"
#include "webrtc/api/videotrack.h"
#include "webrtc/base/bind.h"
#include "webrtc/media/engine/webrtcmediaengine.h"
#include "webrtc/media/engine/webrtcvideodecoderfactory.h"
#include "webrtc/media/engine/webrtcvideoencoderfactory.h"
#include "webrtc/modules/audio_device/include/audio_device.h"
#include "webrtc/p2p/base/basicpacketsocketfactory.h"
#include "webrtc/p2p/client/basicportallocator.h"

namespace webrtc {

namespace {

// Passes down the calls to |store_|. See usage in CreatePeerConnection.
class DtlsIdentityStoreWrapper : public DtlsIdentityStoreInterface {
 public:
  DtlsIdentityStoreWrapper(
      const rtc::scoped_refptr<RefCountedDtlsIdentityStore>& store)
      : store_(store) {
    RTC_DCHECK(store_);
  }

  void RequestIdentity(
      rtc::KeyParams key_params,
      rtc::Optional<uint64_t> expires,
      const rtc::scoped_refptr<webrtc::DtlsIdentityRequestObserver>&
          observer) override {
    store_->RequestIdentity(key_params, expires, observer);
  }

 private:
  rtc::scoped_refptr<RefCountedDtlsIdentityStore> store_;
};

}  // anonymous namespace

rtc::scoped_refptr<PeerConnectionFactoryInterface>
CreatePeerConnectionFactory() {
  rtc::scoped_refptr<PeerConnectionFactory> pc_factory(
      new rtc::RefCountedObject<PeerConnectionFactory>());


  // Call Initialize synchronously but make sure its executed on
  // |signaling_thread|.
  MethodCall0<PeerConnectionFactory, bool> call(
      pc_factory.get(),
      &PeerConnectionFactory::Initialize);
  bool result =  call.Marshal(pc_factory->signaling_thread());

  if (!result) {
    return NULL;
  }
  return PeerConnectionFactoryProxy::Create(pc_factory->signaling_thread(),
                                            pc_factory);
}

rtc::scoped_refptr<PeerConnectionFactoryInterface>
CreatePeerConnectionFactory(
    rtc::Thread* worker_thread,
    rtc::Thread* signaling_thread,
    AudioDeviceModule* default_adm,
    cricket::WebRtcVideoEncoderFactory* encoder_factory,
    cricket::WebRtcVideoDecoderFactory* decoder_factory) {
  rtc::scoped_refptr<PeerConnectionFactory> pc_factory(
      new rtc::RefCountedObject<PeerConnectionFactory>(worker_thread,
                                                       signaling_thread,
                                                       default_adm,
                                                       encoder_factory,
                                                       decoder_factory));

  // Call Initialize synchronously but make sure its executed on
  // |signaling_thread|.
  MethodCall0<PeerConnectionFactory, bool> call(
      pc_factory.get(),
      &PeerConnectionFactory::Initialize);
  bool result =  call.Marshal(signaling_thread);

  if (!result) {
    return NULL;
  }
  return PeerConnectionFactoryProxy::Create(signaling_thread, pc_factory);
}

PeerConnectionFactory::PeerConnectionFactory()
    : owns_ptrs_(true),
      wraps_current_thread_(false),
      signaling_thread_(rtc::ThreadManager::Instance()->CurrentThread()),
      worker_thread_(new rtc::Thread) {
  if (!signaling_thread_) {
    signaling_thread_ = rtc::ThreadManager::Instance()->WrapCurrentThread();
    wraps_current_thread_ = true;
  }
  worker_thread_->Start();
}

PeerConnectionFactory::PeerConnectionFactory(
    rtc::Thread* worker_thread,
    rtc::Thread* signaling_thread,
    AudioDeviceModule* default_adm,
    cricket::WebRtcVideoEncoderFactory* video_encoder_factory,
    cricket::WebRtcVideoDecoderFactory* video_decoder_factory)
    : owns_ptrs_(false),
      wraps_current_thread_(false),
      signaling_thread_(signaling_thread),
      worker_thread_(worker_thread),
      default_adm_(default_adm),
      video_encoder_factory_(video_encoder_factory),
      video_decoder_factory_(video_decoder_factory) {
  ASSERT(worker_thread != NULL);
  ASSERT(signaling_thread != NULL);
  // TODO: Currently there is no way creating an external adm in
  // libjingle source tree. So we can 't currently assert if this is NULL.
  // ASSERT(default_adm != NULL);
}

PeerConnectionFactory::~PeerConnectionFactory() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  channel_manager_.reset(nullptr);

  // Make sure |worker_thread_| and |signaling_thread_| outlive
  // |dtls_identity_store_|, |default_socket_factory_| and
  // |default_network_manager_|.
  dtls_identity_store_ = nullptr;
  default_socket_factory_ = nullptr;
  default_network_manager_ = nullptr;

  if (owns_ptrs_) {
    if (wraps_current_thread_)
      rtc::ThreadManager::Instance()->UnwrapCurrentThread();
    delete worker_thread_;
  }
}

bool PeerConnectionFactory::Initialize() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::InitRandom(rtc::Time());

  default_network_manager_.reset(new rtc::BasicNetworkManager());
  if (!default_network_manager_) {
    return false;
  }

  default_socket_factory_.reset(
      new rtc::BasicPacketSocketFactory(worker_thread_));
  if (!default_socket_factory_) {
    return false;
  }

  // TODO:  Need to make sure only one VoE is created inside
  // WebRtcMediaEngine.
  cricket::MediaEngineInterface* media_engine =
      worker_thread_->Invoke<cricket::MediaEngineInterface*>(rtc::Bind(
      &PeerConnectionFactory::CreateMediaEngine_w, this));

  channel_manager_.reset(
      new cricket::ChannelManager(media_engine, worker_thread_));

  channel_manager_->SetVideoRtxEnabled(true);
  if (!channel_manager_->Init()) {
    return false;
  }

  dtls_identity_store_ = new RefCountedDtlsIdentityStore(
      signaling_thread_, worker_thread_);

  return true;
}

rtc::scoped_refptr<AudioSourceInterface>
PeerConnectionFactory::CreateAudioSource(
    const MediaConstraintsInterface* constraints) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<LocalAudioSource> source(
      LocalAudioSource::Create(options_, constraints));
  return source;
}

rtc::scoped_refptr<AudioSourceInterface>
PeerConnectionFactory::CreateAudioSource(const cricket::AudioOptions& options) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<LocalAudioSource> source(
      LocalAudioSource::Create(options_, &options));
  return source;
}

rtc::scoped_refptr<VideoSourceInterface>
PeerConnectionFactory::CreateVideoSource(
    cricket::VideoCapturer* capturer,
    const MediaConstraintsInterface* constraints) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<VideoSource> source(VideoSource::Create(
      worker_thread_, capturer, constraints, false));
  return VideoSourceProxy::Create(signaling_thread_, source);
}

rtc::scoped_refptr<VideoSourceInterface>
PeerConnectionFactory::CreateVideoSource(cricket::VideoCapturer* capturer) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<VideoSource> source(
      VideoSource::Create(worker_thread_, capturer, false));
  return VideoSourceProxy::Create(signaling_thread_, source);
}

bool PeerConnectionFactory::StartAecDump(rtc::PlatformFile file,
                                         int64_t max_size_bytes) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  return channel_manager_->StartAecDump(file, max_size_bytes);
}

void PeerConnectionFactory::StopAecDump() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  channel_manager_->StopAecDump();
}

bool PeerConnectionFactory::StartRtcEventLog(rtc::PlatformFile file) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  return channel_manager_->StartRtcEventLog(file);
}

void PeerConnectionFactory::StopRtcEventLog() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  channel_manager_->StopRtcEventLog();
}

rtc::scoped_refptr<PeerConnectionInterface>
PeerConnectionFactory::CreatePeerConnection(
    const PeerConnectionInterface::RTCConfiguration& configuration_in,
    const MediaConstraintsInterface* constraints,
    rtc::scoped_ptr<cricket::PortAllocator> allocator,
    rtc::scoped_ptr<DtlsIdentityStoreInterface> dtls_identity_store,
    PeerConnectionObserver* observer) {
  RTC_DCHECK(signaling_thread_->IsCurrent());

  // We merge constraints and configuration into a single configuration.
  PeerConnectionInterface::RTCConfiguration configuration = configuration_in;
  CopyConstraintsIntoRtcConfiguration(constraints, &configuration);

  return CreatePeerConnection(configuration, std::move(allocator),
                              std::move(dtls_identity_store), observer);
}

rtc::scoped_refptr<PeerConnectionInterface>
PeerConnectionFactory::CreatePeerConnection(
    const PeerConnectionInterface::RTCConfiguration& configuration,
    rtc::scoped_ptr<cricket::PortAllocator> allocator,
    rtc::scoped_ptr<DtlsIdentityStoreInterface> dtls_identity_store,
    PeerConnectionObserver* observer) {
  RTC_DCHECK(signaling_thread_->IsCurrent());

  if (!dtls_identity_store.get()) {
    // Because |pc|->Initialize takes ownership of the store we need a new
    // wrapper object that can be deleted without deleting the underlying
    // |dtls_identity_store_|, protecting it from being deleted multiple times.
    dtls_identity_store.reset(
        new DtlsIdentityStoreWrapper(dtls_identity_store_));
  }

  if (!allocator) {
    allocator.reset(new cricket::BasicPortAllocator(
        default_network_manager_.get(), default_socket_factory_.get()));
  }
  allocator->SetNetworkIgnoreMask(options_.network_ignore_mask);

  rtc::scoped_refptr<PeerConnection> pc(
      new rtc::RefCountedObject<PeerConnection>(this));
  // We rely on default values when constraints aren't found.
  cricket::MediaConfig media_config;

  media_config.video.disable_prerenderer_smoothing =
      configuration.disable_prerenderer_smoothing;
  if (configuration.enable_dscp) {
    media_config.enable_dscp = *(configuration.enable_dscp);
  }
  if (configuration.cpu_overuse_detection) {
    media_config.video.enable_cpu_overuse_detection =
        *(configuration.cpu_overuse_detection);
  }
  if (configuration.suspend_below_min_bitrate) {
    media_config.video.suspend_below_min_bitrate =
        *(configuration.suspend_below_min_bitrate);
  }

  if (!pc->Initialize(media_config, configuration, std::move(allocator),
                      std::move(dtls_identity_store), observer)) {
    return nullptr;
  }
  return PeerConnectionProxy::Create(signaling_thread(), pc);
}

rtc::scoped_refptr<MediaStreamInterface>
PeerConnectionFactory::CreateLocalMediaStream(const std::string& label) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  return MediaStreamProxy::Create(signaling_thread_,
                                  MediaStream::Create(label));
}

rtc::scoped_refptr<VideoTrackInterface>
PeerConnectionFactory::CreateVideoTrack(
    const std::string& id,
    VideoSourceInterface* source) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<VideoTrackInterface> track(
      VideoTrack::Create(id, source));
  return VideoTrackProxy::Create(signaling_thread_, track);
}

rtc::scoped_refptr<AudioTrackInterface>
PeerConnectionFactory::CreateAudioTrack(const std::string& id,
                                        AudioSourceInterface* source) {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  rtc::scoped_refptr<AudioTrackInterface> track(AudioTrack::Create(id, source));
  return AudioTrackProxy::Create(signaling_thread_, track);
}

webrtc::MediaControllerInterface* PeerConnectionFactory::CreateMediaController(
    const cricket::MediaConfig& config) const {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  return MediaControllerInterface::Create(config, worker_thread_,
                                          channel_manager_.get());
}

rtc::Thread* PeerConnectionFactory::signaling_thread() {
  // This method can be called on a different thread when the factory is
  // created in CreatePeerConnectionFactory().
  return signaling_thread_;
}

rtc::Thread* PeerConnectionFactory::worker_thread() {
  RTC_DCHECK(signaling_thread_->IsCurrent());
  return worker_thread_;
}

cricket::MediaEngineInterface* PeerConnectionFactory::CreateMediaEngine_w() {
  ASSERT(worker_thread_ == rtc::Thread::Current());
  return cricket::WebRtcMediaEngineFactory::Create(
      default_adm_.get(), video_encoder_factory_.get(),
      video_decoder_factory_.get());
}

}  // namespace webrtc

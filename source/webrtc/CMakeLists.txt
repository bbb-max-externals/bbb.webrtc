add_library(bbb_webrtc STATIC
	src/webrtc_session.cpp
	src/opus_codec.cpp
	src/audio_ring_buffer.cpp
	src/video_session.cpp
	src/color_convert.cpp
)

if(APPLE)
	target_sources(bbb_webrtc PRIVATE
		src/video_encoder_videotoolbox.mm
		src/video_decoder_videotoolbox.mm
	)
	find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox)
	find_library(COREMEDIA_FRAMEWORK CoreMedia)
	find_library(COREVIDEO_FRAMEWORK CoreVideo)
	find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
	find_library(FOUNDATION_FRAMEWORK Foundation)
	target_link_libraries(bbb_webrtc PUBLIC
		${VIDEOTOOLBOX_FRAMEWORK}
		${COREMEDIA_FRAMEWORK}
		${COREVIDEO_FRAMEWORK}
		${COREFOUNDATION_FRAMEWORK}
		${FOUNDATION_FRAMEWORK}
		objc
	)
	target_compile_options(bbb_webrtc PRIVATE -fobjc-arc)
elseif(WIN32)
	target_sources(bbb_webrtc PRIVATE
		src/video_encoder_mf.cpp
		src/video_decoder_mf.cpp
	)
	target_link_libraries(bbb_webrtc PUBLIC
		mfplat
		mf
		uuid
		strmiids
		mfcodec
	)
endif()

target_include_directories(bbb_webrtc PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(bbb_webrtc PUBLIC
	datachannel
	Opus::opus
)

target_compile_features(bbb_webrtc PUBLIC cxx_std_17)

if(WIN32)
	target_compile_definitions(bbb_webrtc PUBLIC
		WEBRTC_STATIC
		WEBRTC_EXPORTS
	)
endif()

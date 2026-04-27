add_library(bbb_webrtc STATIC
	src/webrtc_session.cpp
	src/opus_codec.cpp
	src/audio_ring_buffer.cpp
)

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

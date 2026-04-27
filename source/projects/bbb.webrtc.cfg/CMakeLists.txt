project(bbb.webrtc.cfg)

bbb_add_external(
	DEPS bbb_webrtc
	INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/../../webrtc/include
)

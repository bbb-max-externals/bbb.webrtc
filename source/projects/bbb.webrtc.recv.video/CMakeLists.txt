project(bbb.webrtc.recv.video)

bbb_add_external(
	DEPS bbb_webrtc_video
	INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/../../webrtc/include
)

cmake_minimum_required(VERSION 3.0)

project(ymfm CXX)

add_library(ymfm
	STATIC
	src/ymfm_adpcm.cpp
	src/ymfm_misc.cpp
	src/ymfm_opl.cpp
	src/ymfm_opm.cpp
	src/ymfm_opn.cpp
	src/ymfm_opq.cpp
	src/ymfm_opz.cpp
	src/ymfm_pcm.cpp
	src/ymfm_ssg.cpp
)

target_include_directories(
	ymfm PUBLIC src
)
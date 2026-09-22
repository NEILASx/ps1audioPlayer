cmake_minimum_required(VERSION 3.21)

project(
	ps1audioPlayer
	LANGUAGES    C CXX ASM
	VERSION      1.0.0
	DESCRIPTION  "PS1 audio player"
	HOMEPAGE_URL "https://github.com/NEILASx/ps1audioPlayer"
)

psn00bsdk_add_executable(template GPREL src/main.c)

psn00bsdk_add_cd_image(
	iso      # Target name
	ps1audiooplayer # Output file name (= template.bin + template.cue)
	iso.xml  # Path to config file
	DEPENDS template system.cnf
)
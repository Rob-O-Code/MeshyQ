cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(MeshyQ)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)

generate_inc_file_for_target(
  app
  ${CMAKE_CURRENT_SOURCE_DIR}/ding_48k_mono_s16le.pcm
  ${gen_dir}/ding_pcm.inc
)

target_sources(app PRIVATE
  src/main.c
  src/model_handler.c
  src/chat_cli.c
  src/audio_player.c
  src/cs47l63_comm.c
)

target_include_directories(app PRIVATE
  include
  ${gen_dir}
)

add_subdirectory(${ZEPHYR_CIRRUS_LOGIC_MODULE_DIR} cirrus_logic_bin_dir)

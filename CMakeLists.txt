cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(MeshyQ)

target_sources(app PRIVATE
  src/main.c
  src/model_handler.c
  src/chat_cli.c
  src/audio_player.c
  src/vs1053.c
  src/sd_card.c
)

target_include_directories(app PRIVATE
  include
)

# CMake generated Testfile for 
# Source directory: /home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service
# Build directory: /home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service/build_envcontext
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[homeguardian_tests]=] "/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service/build_envcontext/homeguardian_tests")
set_tests_properties([=[homeguardian_tests]=] PROPERTIES  ENVIRONMENT "HG_SOURCE_DIR=/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service" WORKING_DIRECTORY "/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service" _BACKTRACE_TRIPLES "/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service/CMakeLists.txt;584;add_test;/home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/hermes-mi-a3-edge-service/CMakeLists.txt;0;")
subdirs("_deps/spdlog-build")
subdirs("_deps/nlohmann_json-build")

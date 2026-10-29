include_guard(GLOBAL)
include(FetchContent)
# 消费方已提供目标时直接复用；否则拉取经过验证的固定提交，不启用 Handle 可选模块。
if(NOT TARGET Lidar::HandleCore)
    FetchContent_Declare(lidar_handlecore
        GIT_REPOSITORY https://github.com/Lateef-ZHOU/HandleCore.git
        GIT_TAG 3470a1d9f3826f4a666b0e53c29c001f0901fbbd)
    FetchContent_MakeAvailable(lidar_handlecore)
endif()

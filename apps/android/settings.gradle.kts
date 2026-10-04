// Memex Android 客户端（T6.3）：单 app 模块工程。
// 协议单一事实源在仓库根 common/proto/memex.proto，由 app 模块跨目录引入。
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "memex-android"
include(":app")

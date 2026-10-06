import com.google.protobuf.gradle.proto
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.google.protobuf")
}

// 协议单一事实源：仓库根 common/proto/memex.proto（与服务端、桌面端同一份）。
// 相对层级 apps/android/app → 仓库根 = 三级向上；改层级必须同步改这里。
val protoSourceDir = rootDir.resolve("../../common/proto").canonicalFile
require(protoSourceDir.resolve("memex.proto").exists()) {
    "找不到协议单一事实源：$protoSourceDir/memex.proto"
}

// protoc 与运行时必须同版本；lite 运行时（protobuf-javalite）适配 Android。
val protobufVersion = "4.31.1"

android {
    namespace = "com.memex.im"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.memex.im"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            // 内网自用分发，首版不做混淆（后续 T6.3 收口再上 R8 规则）
            isMinifyEnabled = false
        }
    }

    buildFeatures {
        // BuildConfig.VERSION_NAME 作 LOGIN 载荷的 client_version
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            proto {
                srcDir(protoSourceDir)
            }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

protobuf {
    protoc {
        artifact = "com.google.protobuf:protoc:$protobufVersion"
    }
    generateProtoTasks {
        all().forEach { task ->
            task.builtins {
                // Android 变体下 java builtin 不预注册，显式 create；
                // lite 选项＝生成 lite 代码（与桌面端 C++ full runtime 线格式一致）
                create("java") {
                    option("lite")
                }
            }
        }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.protobuf:protobuf-javalite:$protobufVersion")
    testImplementation("junit:junit:4.13.2")
    // org.json：主代码用 Android 内置实现；JVM 单测的 android.jar 里
    // org.json 是占位桩（调用即 not mocked），用独立 artifact 顶掉同名类
    testImplementation("org.json:json:20240303")
}

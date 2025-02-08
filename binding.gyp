{
    "targets": [
        {
            "target_name": "detection",
            "sources": [
                "src/detection.cpp",
                "src/detection.h",
                "src/deviceList.cpp"
            ],
            "defines": [
                "NODE_ADDON_API_CPP_EXCEPTIONS=1",
                "NAPI_VERSION=8"
            ],
            "include_dirs": [
                "<!@(node -p \"require('node-addon-api').include\")"
            ],
            "cflags_cc": ["-fexceptions"],
            "conditions": [
                [
                    "OS=='win'",
                    {
                        "sources": ["src/detection_win.cpp"],
                        "defines": [
                            "_HAS_EXCEPTIONS=1"
                        ],
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "ExceptionHandling": 1,
                                "RuntimeTypeInfo": "true"
                            }
                        }
                    }
                ],
                [
                    "OS=='mac'",
                    {
                        "sources": ["src/detection_mac.cpp"],
                        "libraries": [
                        "-Wl,-framework,IOKit",
                        "-Wl,-framework,CoreFoundation"
                        ],
                        "default_configuration": "Debug",
                        "configurations": {
                            "Debug": {
                                "defines": ["DEBUG", "_DEBUG"]
                            },
                            "Release": {
                                "defines": ["NDEBUG"]
                            }
                        },
                        "cflags": ["-fvisibility=default"],
                        "xcode_settings": {
                            "MACOSX_DEPLOYMENT_TARGET": "14.0",
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            "CLANG_CXX_LIBRARY": "libc++"
                        }
                    }
                ],
                [
                    "OS=='linux'",
                    {
                        "sources": ["src/detection_linux.cpp"],
                        "link_settings": {
                            "libraries": ["-ludev"]
                        }
                    }
                ]
            ]
        }
    ]
}

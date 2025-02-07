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
                            "-framework",
                            "IOKit"
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
                        "xcode_settings": {
                            "MACOSX_DEPLOYMENT_TARGET": "10.9"
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

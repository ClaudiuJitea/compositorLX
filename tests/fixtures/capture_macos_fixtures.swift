#!/usr/bin/env swift
//
// capture_macos_fixtures.swift
// Standalone Synthetic Apple-Framework Fixture Generator
//
// STATUS: source reviewed, not compiled or run.
//
// PROVENANCE: SYNTHESIZED_MACOS_FRAMEWORKS
//
// IMPORTANT NOTE ON PROVENANCE & RENDERING:
// This script is a synthetic fixture generator that calls Apple's system
// Foundation, CoreGraphics, and ImageIO frameworks directly on macOS (Darwin).
// It does NOT invoke the macOS Compositor application binary, nor does it use
// Compositor's internal rendering engine or EditorSession pipeline.
// Consequently, this standalone script DOES NOT generate genuine Compositor
// reference composites.
//
// For genuine macOS application packages and ground-truth reference composites
// rendered by Compositor's actual ImageExporter / Metal pipeline, run the in-tree
// Xcode test harness:
//   xcodebuild test -project Compositor.xcodeproj -scheme Compositor \
//     -destination 'platform=macOS' \
//     -only-testing:CompositorTests/InterchangeFixtureCaptureTests
//
// Usage:
//   swift capture_macos_fixtures.swift [output_directory]
//

import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let outputDir: URL
if CommandLine.arguments.count > 1 {
    outputDir = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
} else {
    outputDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        .appendingPathComponent("synthetic_apple_fixtures", isDirectory: true)
}

try? FileManager.default.createDirectory(at: outputDir, withIntermediateDirectories: true)
print("[Synthetic Apple-Framework Fixture Generator]")
print("  Output directory: \(outputDir.path)")
print("  Note: Generated packages use Apple system libraries directly for schema syntax testing.")

// Helper to create an 8-bit sRGB PNG with CoreGraphics & ImageIO
func createPngData(width: Int, height: Int, drawing: (CGContext) -> Void) -> Data {
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    let bitmapInfo = CGImageAlphaInfo.premultipliedLast.rawValue
    let context = CGContext(
        data: nil,
        width: width,
        height: height,
        bitsPerComponent: 8,
        bytesPerRow: width * 4,
        space: colorSpace,
        bitmapInfo: bitmapInfo
    )!
    drawing(context)
    guard let cgImage = context.makeImage() else {
        fatalError("Failed to create CGImage")
    }

    let data = NSMutableData()
    guard let destination = CGImageDestinationCreateWithData(
        data,
        UTType.png.identifier as CFString,
        1,
        nil
    ) else {
        fatalError("Failed to create CGImageDestination")
    }
    CGImageDestinationAddImage(destination, cgImage, nil)
    CGImageDestinationFinalize(destination)
    return data as Data
}

// Package writer helper (strictly writes package assets and manifest; no mock composite claim)
func writePackage(
    named name: String,
    manifest: [String: Any],
    images: [String: Data] = [:],
    masks: [String: Data] = [:]
) {
    let packageURL = outputDir.appendingPathComponent("\(name).comp", isDirectory: true)
    let imagesURL = packageURL.appendingPathComponent("images", isDirectory: true)
    try? FileManager.default.createDirectory(at: imagesURL, withIntermediateDirectories: true)

    // Write manifest.json with sorted keys and pretty printing
    let manifestURL = packageURL.appendingPathComponent("manifest.json")
    let jsonData = try! JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
    try! jsonData.write(to: manifestURL)

    // Write image assets
    for (filename, data) in images {
        try! data.write(to: imagesURL.appendingPathComponent(filename))
    }
    for (filename, data) in masks {
        try! data.write(to: imagesURL.appendingPathComponent(filename))
    }

    print("  + Generated synthetic Apple-framework package: \(packageURL.lastPathComponent)")
}

func defaultTransform(w: Double = 64, h: Double = 64, x: Double = 0, y: Double = 0) -> [String: Any] {
    return [
        "flipX": false,
        "flipY": false,
        "origin": [x, y],
        "rotation": 0.0,
        "sampling": "High quality",
        "size": [w, h]
    ]
}

// =============================================================================
// 1. Synthetic Apple-Framework v7 Package (Editable Text & Levels Adjustment)
// =============================================================================
do {
    let docID = UUID().uuidString
    let bgID = UUID().uuidString
    let textID = UUID().uuidString
    let adjID = UUID().uuidString

    let bgData = createPngData(width: 128, height: 128) { ctx in
        ctx.setFillColor(red: 0.15, green: 0.18, blue: 0.22, alpha: 1.0)
        ctx.fill(CGRect(x: 0, y: 0, width: 128, height: 128))
    }

    let textData = createPngData(width: 128, height: 128) { ctx in
        ctx.setFillColor(red: 0.95, green: 0.85, blue: 0.20, alpha: 1.0)
        ctx.fill(CGRect(x: 14, y: 44, width: 100, height: 40))
    }

    let manifest: [String: Any] = [
        "format": "com.compositor.project",
        "version": 7,
        "colorSpace": "sRGB",
        "documentID": docID,
        "width": 128,
        "height": 128,
        "resolution": 72.0,
        "activeLayerID": textID,
        "layers": [
            [
                "id": bgID,
                "name": "Background",
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Normal",
                "imageFile": "\(bgID).png",
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": textID,
                "name": "TextLayer",
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Normal",
                "imageFile": "\(textID).png",
                "text": [
                    "content": "Synthetic Apple Framework Headline",
                    "fontName": "Helvetica",
                    "fontSize": 18.0,
                    "alignment": "Center",
                    "tracking": 2.5,
                    "leading": 22.0,
                    "boxSize": [100.0, 40.0],
                    "red": 0.95,
                    "green": 0.85,
                    "blue": 0.20
                ],
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": adjID,
                "name": "Levels Adjustment",
                "isVisible": true,
                "opacity": 0.9,
                "blendMode": "Normal",
                "adjustment": [
                    "kind": "Levels",
                    "levels": [
                        "channel": "RGB",
                        "ranges": [
                            [
                                "black": 10.0,
                                "gamma": 1.15,
                                "outputBlack": 0.0,
                                "outputWhite": 255.0,
                                "white": 245.0
                            ],
                            [
                                "black": 0.0,
                                "gamma": 1.0,
                                "outputBlack": 0.0,
                                "outputWhite": 255.0,
                                "white": 255.0
                            ],
                            [
                                "black": 0.0,
                                "gamma": 1.0,
                                "outputBlack": 0.0,
                                "outputWhite": 255.0,
                                "white": 255.0
                            ],
                            [
                                "black": 0.0,
                                "gamma": 1.0,
                                "outputBlack": 0.0,
                                "outputWhite": 255.0,
                                "white": 255.0
                            ]
                        ]
                    ]
                ],
                "transform": defaultTransform(w: 128, h: 128)
            ]
        ]
    ]

    writePackage(
        named: "synthetic_apple_v7_text_adjustments",
        manifest: manifest,
        images: ["\(bgID).png": bgData, "\(textID).png": textData]
    )
}

// =============================================================================
// 2. Synthetic Apple-Framework v8 Package (Dimmed Folder and Guides)
// =============================================================================
do {
    let docID = UUID().uuidString
    let bgID = UUID().uuidString
    let groupID = UUID().uuidString
    let childID = UUID().uuidString
    let guide1ID = UUID().uuidString
    let guide2ID = UUID().uuidString

    let bgData = createPngData(width: 128, height: 128) { ctx in
        ctx.setFillColor(red: 0.9, green: 0.9, blue: 0.9, alpha: 1.0)
        ctx.fill(CGRect(x: 0, y: 0, width: 128, height: 128))
    }

    let childData = createPngData(width: 64, height: 64) { ctx in
        ctx.setFillColor(red: 0.2, green: 0.5, blue: 0.9, alpha: 1.0)
        ctx.fill(CGRect(x: 0, y: 0, width: 64, height: 64))
    }

    let manifest: [String: Any] = [
        "format": "com.compositor.project",
        "version": 8,
        "colorSpace": "sRGB",
        "documentID": docID,
        "width": 128,
        "height": 128,
        "resolution": 144.0,
        "activeLayerID": childID,
        "guides": [
            ["id": guide1ID, "axis": "horizontal", "position": 64.0],
            ["id": guide2ID, "axis": "vertical", "position": 64.0]
        ],
        "layers": [
            [
                "id": bgID,
                "name": "Base Canvas",
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Normal",
                "imageFile": "\(bgID).png",
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": groupID,
                "name": "Dimmed Folder",
                "isGroup": true,
                "isVisible": true,
                "opacity": 0.65,
                "blendMode": "Normal",
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": childID,
                "name": "Group Child",
                "parentID": groupID,
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Multiply",
                "imageFile": "\(childID).png",
                "transform": defaultTransform(w: 64, h: 64, x: 32, y: 32)
            ]
        ]
    ]

    writePackage(
        named: "synthetic_apple_v8_folder_guides",
        manifest: manifest,
        images: ["\(bgID).png": bgData, "\(childID).png": childData]
    )
}

// =============================================================================
// 3. Synthetic Apple-Framework v9 Package (Effects, Blurs, Noise)
// =============================================================================
do {
    let docID = UUID().uuidString
    let bgID = UUID().uuidString
    let badgeID = UUID().uuidString
    let blurAdjID = UUID().uuidString
    let noiseAdjID = UUID().uuidString

    let bgData = createPngData(width: 128, height: 128) { ctx in
        ctx.setFillColor(red: 0.1, green: 0.1, blue: 0.15, alpha: 1.0)
        ctx.fill(CGRect(x: 0, y: 0, width: 128, height: 128))
    }

    let badgeData = createPngData(width: 64, height: 64) { ctx in
        ctx.setFillColor(red: 0.9, green: 0.3, blue: 0.2, alpha: 1.0)
        ctx.fillEllipse(in: CGRect(x: 8, y: 8, width: 48, height: 48))
    }

    let manifest: [String: Any] = [
        "format": "com.compositor.project",
        "version": 9,
        "colorSpace": "sRGB",
        "documentID": docID,
        "width": 128,
        "height": 128,
        "resolution": 72.0,
        "activeLayerID": badgeID,
        "layers": [
            [
                "id": bgID,
                "name": "Background",
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Normal",
                "imageFile": "\(bgID).png",
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": badgeID,
                "name": "Badge with Effects",
                "isVisible": true,
                "opacity": 1.0,
                "blendMode": "Normal",
                "imageFile": "\(badgeID).png",
                "effects": [
                    "stroke": [
                        "enabled": true,
                        "size": 4.0,
                        "inside": false,
                        "opacity": 0.9,
                        "red": 1.0,
                        "green": 0.8,
                        "blue": 0.2
                    ],
                    "shadow": [
                        "enabled": true,
                        "blur": 12.0,
                        "distance": 8.0,
                        "angle": 45.0,
                        "opacity": 0.7,
                        "red": 0.0,
                        "green": 0.0,
                        "blue": 0.0
                    ],
                    "outerGlow": [
                        "enabled": true,
                        "size": 16.0,
                        "opacity": 0.6,
                        "red": 1.0,
                        "green": 0.4,
                        "blue": 0.3
                    ]
                ],
                "transform": defaultTransform(w: 64, h: 64, x: 32, y: 32)
            ],
            [
                "id": blurAdjID,
                "name": "Gaussian Blur",
                "isVisible": true,
                "opacity": 0.8,
                "blendMode": "Normal",
                "adjustment": [
                    "kind": "Gaussian Blur",
                    "blurRadius": 4.5
                ],
                "transform": defaultTransform(w: 128, h: 128)
            ],
            [
                "id": noiseAdjID,
                "name": "Add Noise",
                "isVisible": true,
                "opacity": 0.5,
                "blendMode": "Overlay",
                "adjustment": [
                    "kind": "Add Noise",
                    "noiseAmount": 20.0,
                    "noiseGaussian": true,
                    "noiseMonochromatic": true,
                    "noiseSeed": 424242
                ],
                "transform": defaultTransform(w: 128, h: 128)
            ]
        ]
    ]

    writePackage(
        named: "synthetic_apple_v9_effects_blurs_noise",
        manifest: manifest,
        images: ["\(bgID).png": bgData, "\(badgeID).png": badgeData]
    )
}

print("[Synthetic Apple-Framework Fixture Generator] Complete. 3 synthetic packages generated for schema testing.")

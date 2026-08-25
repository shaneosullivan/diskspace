// Renders the installer window background for the DMG.
//
// Drawn rather than shipped as a binary asset so the wording, colours and arrow
// stay in step with the window geometry the export script sets. Rendered at 2x
// and tagged 144 dpi, so Finder maps it to the 600x320pt window.
import AppKit

let scale = 2
let width = 600 * scale
let height = 320 * scale
let output = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "dmg-background.png"

guard
    let ctx = CGContext(
        data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
        space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
else { exit(1) }

ctx.scaleBy(x: CGFloat(scale), y: CGFloat(scale))
let w = 600.0, h = 320.0

// A soft vertical wash, light enough that dark icon labels stay readable.
let colours =
    [
        NSColor(srgbRed: 0.96, green: 0.97, blue: 0.99, alpha: 1).cgColor,
        NSColor(srgbRed: 0.87, green: 0.90, blue: 0.95, alpha: 1).cgColor,
    ] as CFArray
if let gradient = CGGradient(
    colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colours, locations: [0, 1])
{
    ctx.drawLinearGradient(
        gradient, start: CGPoint(x: 0, y: h), end: CGPoint(x: 0, y: 0), options: [])
}

// Arrow between the two icons. CoreGraphics puts the origin bottom-left, while
// Finder positions icons from the top, so this sits at h - 165 to line up with
// the icon row.
let arrowY = h - 165
let arrowStart = 258.0
let arrowEnd = 348.0
ctx.setStrokeColor(NSColor(srgbRed: 0.42, green: 0.47, blue: 0.56, alpha: 1).cgColor)
ctx.setLineWidth(6)
ctx.setLineCap(.round)
ctx.move(to: CGPoint(x: arrowStart, y: arrowY))
ctx.addLine(to: CGPoint(x: arrowEnd - 12, y: arrowY))
ctx.strokePath()

ctx.setFillColor(NSColor(srgbRed: 0.42, green: 0.47, blue: 0.56, alpha: 1).cgColor)
ctx.move(to: CGPoint(x: arrowEnd + 8, y: arrowY))
ctx.addLine(to: CGPoint(x: arrowEnd - 16, y: arrowY + 14))
ctx.addLine(to: CGPoint(x: arrowEnd - 16, y: arrowY - 14))
ctx.closePath()
ctx.fillPath()

func draw(_ text: String, size: CGFloat, weight: NSFont.Weight, y: CGFloat, alpha: CGFloat) {
    let attributes: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: size, weight: weight),
        .foregroundColor: NSColor(srgbRed: 0.16, green: 0.19, blue: 0.24, alpha: alpha),
    ]
    let string = NSAttributedString(string: text, attributes: attributes)
    let bounds = string.size()
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
    string.draw(at: CGPoint(x: (w - bounds.width) / 2, y: y))
    NSGraphicsContext.restoreGraphicsState()
}

draw("Install Disk Space", size: 22, weight: .semibold, y: h - 58, alpha: 1)
draw(
    "Drag the app onto the Applications folder", size: 13, weight: .regular, y: h - 84,
    alpha: 0.65)

guard let image = ctx.makeImage() else { exit(1) }
let rep = NSBitmapImageRep(cgImage: image)
guard let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
try? png.write(to: URL(fileURLWithPath: output))
print("wrote \(output) (\(width)x\(height))")

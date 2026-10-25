// The picture behind the DMG window: the app on the left, an arrow, Applications on the
// right, and what to do. Finder puts the two icons on it (tools/make-dmg.sh positions
// them at the centres used here).
//
//   swift packaging/macos/dmg-background.swift OUT.png SCALE
//
// make-dmg.sh renders it at 1x and 2x and joins them into one Retina-ready TIFF.
import AppKit

// 428 tall: the window is 660 x 428 whether or not Finder counts its title bar in that.
let width: CGFloat = 660, height: CGFloat = 428
let appCentre = CGPoint( x: 180, y: 200 ), applicationsCentre = CGPoint( x: 480, y: 200 )

let args = CommandLine.arguments
guard args.count == 3, let scale = Double( args[2] ) else
{
    FileHandle.standardError.write( "usage: dmg-background.swift OUT.png SCALE\n".data( using: .utf8 )! )
    exit( 2 )
}

let rep = NSBitmapImageRep( bitmapDataPlanes: nil, pixelsWide: Int( width * scale ), pixelsHigh: Int( height * scale ),
                            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0 )!
rep.size = NSSize( width: width, height: height )
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext( bitmapImageRep: rep )

// Drawn top-down, as Finder places icons.
let flip = NSAffineTransform()
flip.translateX( by: 0, yBy: height )
flip.scaleX( by: 1, yBy: -1 )
flip.concat()

NSColor( calibratedRed: 0.969, green: 0.969, blue: 0.961, alpha: 1 ).setFill()
NSRect( x: 0, y: 0, width: width, height: height ).fill()

func text( _ string: String, size: CGFloat, weight: NSFont.Weight, gray: CGFloat, centreY: CGFloat )
{
    let attributes: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont( ofSize: size, weight: weight ),
        .foregroundColor: NSColor( calibratedWhite: gray, alpha: 1 ) ]
    let s = NSAttributedString( string: string, attributes: attributes )
    let bounds = s.size()
    // Text draws upright in a flipped context only through a flipped graphics context.
    NSGraphicsContext.current = NSGraphicsContext( cgContext: NSGraphicsContext.current!.cgContext, flipped: true )
    s.draw( at: NSPoint( x: ( width - bounds.width ) / 2, y: centreY - bounds.height / 2 ) )
}

text( "Install Inductive", size: 22, weight: .semibold, gray: 0.08, centreY: 58 )
text( "Drag Inductive onto the Applications folder", size: 13, weight: .regular, gray: 0.42, centreY: 88 )
text( "Then open Inductive from Applications or Launchpad.", size: 12, weight: .regular, gray: 0.55, centreY: 352 )

// The arrow, between where the two icons' edges will be.
let y = appCentre.y, from = appCentre.x + 84, to = applicationsCentre.x - 84
let arrow = NSBezierPath()
arrow.move( to: NSPoint( x: from, y: y ) )
arrow.line( to: NSPoint( x: to, y: y ) )
arrow.move( to: NSPoint( x: to - 14, y: y - 12 ) )
arrow.line( to: NSPoint( x: to, y: y ) )
arrow.line( to: NSPoint( x: to - 14, y: y + 12 ) )
arrow.lineWidth = 3
arrow.lineCapStyle = .round
arrow.lineJoinStyle = .round
NSColor( calibratedWhite: 0.16, alpha: 1 ).setStroke()
arrow.stroke()

NSGraphicsContext.restoreGraphicsState()
try! rep.representation( using: .png, properties: [:] )!.write( to: URL( fileURLWithPath: args[1] ) )

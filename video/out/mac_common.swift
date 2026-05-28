/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa
import QuartzCore

class MacCommon: Common {
    @objc var layer: MetalLayer?

    var presentation: Presentation?
    var timer: PreciseTimer?
    var swapTime: UInt64 = 0
    let swapLock: NSCondition = NSCondition()
    var edrSurfacePulsePending: Bool = false
    var edrSurfaceNeedsActivationPulse: Bool = true

    @objc init(_ vo: UnsafeMutablePointer<vo>) {
        let log = LogHelper(mp_log_new(vo, vo.pointee.log, "mac"))
        let option = OptionHelper(vo, vo.pointee.global)
        super.init(option, log)
        eventsLock.withLock { self.vo = vo }
        input = InputHelper(vo.pointee.input_ctx, option)
        presentation = Presentation(common: self)
        timer = PreciseTimer(common: self)

        DispatchQueue.main.sync {
            layer = MetalLayer(common: self)
            initMisc(vo)
        }
    }

    @objc func config(_ vo: UnsafeMutablePointer<vo>) -> Bool {
        eventsLock.withLock { self.vo = vo }

        DispatchQueue.main.sync {
            let previousActiveApp = getActiveApp()
            initApp()

            let (screen, wr, forcePosition) = getInitProperties(vo)
            guard let layer = self.layer else {
                log.error("Something went wrong, no MetalLayer was initialized")
                exit(1)
            }

            if window == nil {
                initView(vo, layer)
                initWindow(vo, previousActiveApp)
                initWindowState()
            }

            if forcePosition {
                window?.updateFrame(wr, screen)
            } else if option.vo.auto_window_resize {
                window?.updateSize(wr.size)
            }

            if option.vo.focus_on == 2 {
                NSApp.activate(ignoringOtherApps: true)
            }

            windowDidResize()
            updateICCProfile()
        }

        return true
    }

    @objc func uninit(_ vo: UnsafeMutablePointer<vo>) {
        window?.waitForAnimation()

        timer?.terminate()

        DispatchQueue.main.sync {
            window?.delegate = nil
            window?.close()

            uninitCommon()
        }
    }

    @objc func swapBuffer() {
        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            swapLock.lock()
            while swapTime < 1 {
                swapLock.wait()
            }
            swapTime = 0
            swapLock.unlock()
        }
    }

    @objc func fillVsync(info: UnsafeMutablePointer<vo_vsync_info>) {
        if option.mac.macos_render_timer != RENDER_TIMER_PRESENTATION_FEEDBACK { return }

        let next = presentation?.next()
        info.pointee.vsync_duration = next?.duration ?? -1
        info.pointee.skipped_vsyncs = next?.skipped ?? -1
        info.pointee.last_queue_display_time = next?.time ?? -1
    }

    @objc func isVisible() -> Bool {
        return window?.occlusionState.contains(.visible) ?? false ||
               option.vo.force_render ||
               needsInitialDraw
    }

    @objc func pulseSurfaceForEDRActivation() {
        // After a swapchain HDR transition, WindowServer may keep treating the
        // already-visible Metal surface as SDR until its owning view changes.
        // Moving the view origin by one backing pixel and restoring it forces
        // that re-evaluation without resizing the window or drawable.
        let pulse = {
            guard let layer = self.layer,
                  let view = self.view,
                  let window = self.window else {
                return
            }

            if !layer.wantsExtendedDynamicRangeContent {
                self.edrSurfaceNeedsActivationPulse = true
                return
            }

            guard self.edrSurfaceNeedsActivationPulse,
                  !self.edrSurfacePulsePending,
                  !window.isMiniaturized else {
                return
            }

            let originalFrame = view.frame
            var pulseFrame = originalFrame
            let delta = 1.0 / max(window.backingScaleFactor, 1.0)
            pulseFrame.origin.x += delta

            self.edrSurfaceNeedsActivationPulse = false
            self.edrSurfacePulsePending = true
            view.frame = pulseFrame
            view.layoutSubtreeIfNeeded()
            view.displayIfNeeded()
            layer.setNeedsLayout()
            layer.layoutIfNeeded()
            CATransaction.flush()
            self.log.verbose("Metal layer EDR surface pulse applied")

            DispatchQueue.main.async { [weak self, weak view, weak layer] in
                guard let self = self else { return }
                defer { self.edrSurfacePulsePending = false }
                guard let view = view, let layer = layer else { return }

                view.frame = originalFrame
                view.layoutSubtreeIfNeeded()
                view.displayIfNeeded()
                layer.setNeedsLayout()
                layer.layoutIfNeeded()
                CATransaction.flush()
                self.log.verbose("Metal layer EDR surface pulse restored")
            }
        }

        if Thread.isMainThread {
            pulse()
        } else {
            DispatchQueue.main.sync(execute: pulse)
        }
    }

    @objc func update(alpha: Bool) {
        layer?.wantsAlpha = alpha
        DispatchQueue.main.sync {
            window?.isOpaque = !alpha
            window?.backgroundColor = alpha ? NSColor.clear : nil
        }
    }

    override func displayLinkCallback(_ displayLink: CVDisplayLink,
                                      _ inNow: UnsafePointer<CVTimeStamp>,
                                      _ inOutputTime: UnsafePointer<CVTimeStamp>,
                                      _ flagsIn: CVOptionFlags,
                                      _ flagsOut: UnsafeMutablePointer<CVOptionFlags>) -> CVReturn {
        let signalSwap = {
            self.swapLock.lock()
            self.swapTime += 1
            self.swapLock.signal()
            self.swapLock.unlock()
        }

        if option.mac.macos_render_timer > RENDER_TIMER_SYSTEM {
            if let timer = self.timer, option.mac.macos_render_timer == RENDER_TIMER_PRECISE {
                timer.scheduleAt(time: inOutputTime.pointee.hostTime, closure: signalSwap)
                return kCVReturnSuccess
            }

            signalSwap()
            return kCVReturnSuccess
        }

        if option.mac.macos_render_timer == RENDER_TIMER_PRESENTATION_FEEDBACK {
            presentation?.add(time: inOutputTime.pointee)
        }

        return kCVReturnSuccess
    }

    override func startDisplayLink(_ vo: UnsafeMutablePointer<vo>) {
        super.startDisplayLink(vo)
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateDisplaylink() {
        super.updateDisplaylink()
        timer?.updatePolicy(periodSeconds: 1 / currentFps())
    }

    override func updateICCProfile() {
        flagEvents(VO_EVENT_ICC_PROFILE_CHANGED)
    }

    override func windowDidResize() {
        flagEvents(VO_EVENT_RESIZE | VO_EVENT_EXPOSE)
    }

    override func windowDidChangeScreenProfile() {
        updateICCProfile()
    }

    override func windowDidChangeBackingProperties() {
        layer?.contentsScale = window?.backingScaleFactor ?? 1
        windowDidResize()
    }

    override func windowDidChangeOcclusionState() {
        flagEvents(VO_EVENT_EXPOSE)
    }
}

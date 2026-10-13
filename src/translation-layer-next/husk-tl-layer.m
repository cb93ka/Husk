/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A game's window whose buffers are smaller than the screen (SurfaceHolder.setFixedSize): its CAMetalLayer gets drawables of that size, which
 * Core Animation stretches over the layer's bounds as Android's compositor would. ANGLE sizes its drawables as bounds x contentsScale, so the
 * scale is what changes; on the main thread, as CALayer wants.
 */
#import <QuartzCore/QuartzCore.h>
#include "husk-tl-bionic.h"

void tl_layer_fit(void *layerp, int w, int h)
{
    if (!layerp || w <= 0 || h <= 0) return;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layerp;
    if (![layer isKindOfClass:[CAMetalLayer class]]) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        CGSize b = layer.bounds.size;
        if (b.width <= 0 || b.height <= 0) return;
        layer.contentsGravity = kCAGravityResize;
        layer.contentsScale = (CGFloat)h / b.height;
        layer.drawableSize = CGSizeMake(w, h);
        tl_log_line("window: layer %.0fx%.0f pt now draws %dx%d (scale %.3f)", b.width, b.height, w, h, layer.contentsScale);
    });
}

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
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libplacebo/config.h>                   // for PL_HAVE_OPENGL, PL_API_VER

#ifdef PL_HAVE_D3D11
#include <libplacebo/d3d11.h>
#endif

#ifdef PL_HAVE_OPENGL
#include "mpv/render_gl.h"                       // for mpv_opengl_init_params
#include <libplacebo/opengl.h>                   // for pl_opengl_destroy
#include "video/out/gpu_next/libmpv_gpu_next.h"  // for libmpv_gpu_next_context
#include "video/out/gpu_next/ra.h"               // for ra_pl_create, ra_pl_...
#include "video/out/placebo/ra_pl.h"             // for ra_create_pl (old RA)
#include "video/out/gpu/ra.h"                    // for ra_free
#endif

#ifdef PL_HAVE_VULKAN
#include "mpv/render_vk.h"                       // for mpv_vulkan_init_params
#include <libplacebo/vulkan.h>                   // for pl_vulkan_import
#include "video/out/gpu_next/libmpv_gpu_next.h"  // for libmpv_gpu_next_context
#include "video/out/gpu_next/ra.h"               // for ra_pl_create, ra_pl_...
#include "video/out/placebo/ra_pl.h"             // for ra_create_pl (old RA)
#include "video/out/gpu/ra.h"                    // for ra_free, ra_add_native_resource
#include "video/out/vulkan/common.h"             // for struct mpvk_ctx
#endif

#include <stddef.h>                              // for NULL
#include "config.h"                              // for HAVE_GL, HAVE_D3D11
#include "context.h"                             // for gpu_ctx
#include "common/msg.h"                          // for MP_ERR, mp_msg, mp_msg_err
#include "mpv/client.h"                          // for mpv_error
#include "mpv/render.h"                          // for mpv_render_param
#include "options/options.h"                     // for mp_vo_opts
#include "ta/ta_talloc.h"                        // for talloc_zero, talloc_...
#include "video/out/gpu/context.h"               // for ra_ctx_opts, ra_ctx
#include "video/out/libmpv.h"                    // for get_mpv_render_param
#include "video/out/opengl/common.h"             // for GL
#include "video/out/placebo/utils.h"             // for mppl_log_set_probing
#include "video/out/vo.h"                        // for vo

#if HAVE_D3D11
#include "osdep/windows_utils.h"
#include "video/out/d3d11/ra_d3d11.h"
#include "video/out/d3d11/context.h"
#endif

#if HAVE_GL
#include "video/out/opengl/ra_gl.h"              // for ra_is_gl, ra_gl_get
# if HAVE_EGL
#include <EGL/egl.h>                             // for eglGetCurrentContext
# endif
#endif

#if HAVE_VULKAN
#include "video/out/vulkan/context.h"            // for ra_vk_ctx_get
#endif

#if HAVE_GL
// Store Libplacebo OpenGL context information.
struct priv {
    pl_log pl_log;
    pl_opengl gl;
    pl_gpu gpu;
    struct ra_next *ra;

    // Store a persistent copy of the init params to avoid a dangling pointer.
    mpv_opengl_init_params gl_params;
};
#endif

#if HAVE_D3D11
static bool d3d11_pl_init(struct vo *vo, struct gpu_ctx *ctx,
                          struct ra_ctx_opts *ctx_opts)
{
#if !defined(PL_HAVE_D3D11)
    MP_MSG(ctx, vo->probing ? MSGL_V : MSGL_ERR,
           "libplacebo was built without D3D11 support.\n");
    return false;
#else // defined(PL_HAVE_D3D11)
    bool success = false;

    ID3D11Device   *device    = ra_d3d11_get_device(ctx->ra_ctx->ra);
    IDXGISwapChain *swapchain = ra_d3d11_ctx_get_swapchain(ctx->ra_ctx);
    if (!device || !swapchain) {
        mp_err(ctx->log,
               "Failed to receive required components from the mpv d3d11 "
               "context! (device: %s, swap chain: %s)\n",
               device    ? "OK" : "failed",
               swapchain ? "OK" : "failed");
        goto err_out;
    }

    pl_d3d11 d3d11 = pl_d3d11_create(ctx->pllog,
        pl_d3d11_params(
            .device = device,
        )
    );
    if (!d3d11) {
        mp_err(ctx->log, "Failed to acquire a d3d11 libplacebo context!\n");
        goto err_out;
    }
    ctx->gpu = d3d11->gpu;

    mppl_log_set_probing(ctx->pllog, false);

    ctx->swapchain = pl_d3d11_create_swapchain(d3d11,
        pl_d3d11_swapchain_params(
            .swapchain = swapchain,
            .disable_10bit_sdr = ra_d3d11_ctx_prefer_8bit_output_format(ctx->ra_ctx),
        )
    );
    if (!ctx->swapchain) {
        mp_err(ctx->log, "Failed to acquire a d3d11 libplacebo swap chain!\n");
        goto err_out;
    }

    success = true;

err_out:
    SAFE_RELEASE(swapchain);
    SAFE_RELEASE(device);

    return success;
#endif // defined(PL_HAVE_D3D11)
}
#endif // HAVE_D3D11

struct gpu_ctx *gpu_ctx_create(struct vo *vo, struct ra_ctx_opts *ctx_opts)
{
    struct gpu_ctx *ctx = talloc_zero(NULL, struct gpu_ctx);
    ctx->log = vo->log;
    ctx->ra_ctx = ra_ctx_create(vo, *ctx_opts);
    if (!ctx->ra_ctx)
        goto err_out;

#if HAVE_VULKAN
    struct mpvk_ctx *vkctx = ra_vk_ctx_get(ctx->ra_ctx);
    if (vkctx) {
        ctx->pllog = vkctx->pllog;
        ctx->gpu = vkctx->gpu;
        ctx->swapchain = vkctx->swapchain;
        return ctx;
    }
#endif

    ctx->pllog = mppl_log_create(ctx, ctx->log);
    if (!ctx->pllog)
        goto err_out;

    mppl_log_set_probing(ctx->pllog, vo->probing);

#if HAVE_D3D11
    if (ra_is_d3d11(ctx->ra_ctx->ra)) {
        if (!d3d11_pl_init(vo, ctx, ctx_opts))
            goto err_out;

        return ctx;
    }
#endif

#if HAVE_GL && defined(PL_HAVE_OPENGL)
    if (ra_is_gl(ctx->ra_ctx->ra)) {
        struct GL *gl = ra_gl_get(ctx->ra_ctx->ra);
        struct pl_opengl_params params = *pl_opengl_params(
            .debug = ctx_opts->debug,
            .allow_software = ctx_opts->allow_sw,
            .get_proc_addr_ex = (void *) gl->get_fn,
            .proc_ctx = gl->fn_ctx,
        );
# if HAVE_EGL
        params.egl_display = eglGetCurrentDisplay();
        params.egl_context = eglGetCurrentContext();
# endif
        pl_opengl opengl = pl_opengl_create(ctx->pllog, &params);
        if (!opengl)
            goto err_out;
        ctx->gpu = opengl->gpu;

        mppl_log_set_probing(ctx->pllog, false);

        ctx->swapchain = pl_opengl_create_swapchain(opengl, pl_opengl_swapchain_params(
            .max_swapchain_depth = vo->opts->swapchain_depth,
            .framebuffer.flipped = gl->flipped,
        ));
        if (!ctx->swapchain)
            goto err_out;

        return ctx;
    }
#elif HAVE_GL
    if (ra_is_gl(ctx->ra_ctx->ra)) {
        MP_MSG(ctx, vo->probing ? MSGL_V : MSGL_ERR,
            "libplacebo was built without OpenGL support.\n");
    }
#endif

err_out:
    gpu_ctx_destroy(&ctx);
    return NULL;
}

bool gpu_ctx_resize(struct gpu_ctx *ctx, int w, int h)
{
#if HAVE_VULKAN
    if (ra_vk_ctx_get(ctx->ra_ctx))
        // vulkan RA handles this by itself
        return true;
#endif

    return pl_swapchain_resize(ctx->swapchain, &w, &h);
}

void gpu_ctx_destroy(struct gpu_ctx **ctxp)
{
    struct gpu_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    if (!ctx->ra_ctx)
        goto skip_common_pl_cleanup;

#if HAVE_VULKAN
    if (ra_vk_ctx_get(ctx->ra_ctx))
        // vulkan RA context handles pl cleanup by itself,
        // skip common local clean-up.
        goto skip_common_pl_cleanup;
#endif

    if (ctx->swapchain)
        pl_swapchain_destroy(&ctx->swapchain);

    if (ctx->gpu) {
#if HAVE_GL && defined(PL_HAVE_OPENGL)
        if (ra_is_gl(ctx->ra_ctx->ra)) {
            pl_opengl opengl = pl_opengl_get(ctx->gpu);
            pl_opengl_destroy(&opengl);
        }
#endif

#if HAVE_D3D11 && defined(PL_HAVE_D3D11)
        if (ra_is_d3d11(ctx->ra_ctx->ra)) {
            pl_d3d11 d3d11 = pl_d3d11_get(ctx->gpu);
            pl_d3d11_destroy(&d3d11);
        }
#endif
    }

    if (ctx->pllog)
        pl_log_destroy(&ctx->pllog);

skip_common_pl_cleanup:
    ra_ctx_destroy(&ctx->ra_ctx);

    talloc_free(ctx);
    *ctxp = NULL;
}

#if HAVE_GL && defined(PL_HAVE_OPENGL)
/**
 * @brief Callback to make the OpenGL context current.
 * @param priv Pointer to the private data (mpv_opengl_init_params).
 * @return True on success, false on failure.
 */
static bool pl_callback_makecurrent_gl(void *priv)
{
    mpv_opengl_init_params *gl_params = priv;
    // The mpv render API contract specifies that the client must make the
    // context current inside its get_proc_address callback. We can trigger
    // this by calling it with a harmless, common function name.
    if (gl_params && gl_params->get_proc_address) {
        gl_params->get_proc_address(gl_params->get_proc_address_ctx, "glGetString");
        return true;
    }

    return false;
}

/**
 * @brief Callback to release the OpenGL context.
 * @param priv Pointer to the private data (mpv_opengl_init_params).
 */
static void pl_callback_releasecurrent_gl(void *priv)
{
}

/**
 * @brief Callback to log messages from libplacebo.
 * @param log_priv Pointer to the private data (mp_log).
 * @param level The log level.
 * @param msg The log message.
 */
static void pl_log_cb(void *log_priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = log_priv;
    mp_msg(log, MSGL_WARN, "[gpu-next:pl] %s\n", msg);
}

/**
 * @brief Initializes the OpenGL context for the GPU next renderer.
 * @param ctx The libmpv_gpu_next_context to initialize.
 * @param params The render parameters.
 * @return 0 on success, negative error code on failure.
 */
static int libmpv_gpu_next_init_gl(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_opengl_init_params *gl_params =
    get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, NULL);
    if (!gl_params || !gl_params->get_proc_address)
        return MPV_ERROR_INVALID_PARAMETER;

    // Make a persistent copy of the params struct's contents.
    p->gl_params = *gl_params;

    // Setup libplacebo logging
    struct pl_log_params log_params = {
        .log_level = PL_LOG_DEBUG
    };

    // Enable verbose logging if trace is enabled
    if (mp_msg_test(ctx->log, MSGL_TRACE)) {
        log_params.log_cb = pl_log_cb;
        log_params.log_priv = ctx->log;
    }

    p->pl_log = pl_log_create(PL_API_VER, &log_params);
    p->gl = pl_opengl_create(p->pl_log, pl_opengl_params(
        .get_proc_addr_ex = (pl_voidfunc_t (*)(void*, const char*))gl_params->get_proc_address,
        .proc_ctx = gl_params->get_proc_address_ctx,
        .make_current = pl_callback_makecurrent_gl,
        .release_current = pl_callback_releasecurrent_gl,
        .priv = &p->gl_params // Pass the ADDRESS of our persistent copy
    ));

    if (!p->gl) {
        MP_ERR(ctx, "Failed to create libplacebo OpenGL context.\n");
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_UNSUPPORTED;
    }
    p->gpu = p->gl->gpu;

    // Pass the libplacebo log to the RA as well.
    p->ra = ra_pl_create(p->gpu, ctx->log, p->pl_log);
    if (!p->ra) {
        pl_opengl_destroy(&p->gl);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    // Create old-style RA for hwdec infrastructure (wraps same pl_gpu)
    ctx->old_ra = ra_create_pl(p->gpu, mp_log_new(ctx, ctx->log, "old-ra"));
    if (!ctx->old_ra) {
        MP_ERR(ctx, "Failed to create old RA for hwdec\n");
        ra_pl_destroy(&p->ra);
        pl_opengl_destroy(&p->gl);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    ctx->ra = p->ra;
    ctx->gpu = p->gpu;
    return 0;
}

/**
 * @brief Wraps an OpenGL framebuffer object (FBO) as a libplacebo texture.
 * @param ctx The libmpv_gpu_next_context.
 * @param params The render parameters.
 * @param out_tex Pointer to the output texture.
 * @return 0 on success, negative error code on failure.
 */
static int libmpv_gpu_next_wrap_fbo_gl(struct libmpv_gpu_next_context *ctx,
                    mpv_render_param *params, pl_tex *out_tex)
{
    struct priv *p = ctx->priv;
    *out_tex = NULL;

    // Get the FBO from the render parameters
    mpv_opengl_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_OPENGL_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    // Wrap the FBO as a libplacebo texture
    pl_tex tex = pl_opengl_wrap(p->gpu, pl_opengl_wrap_params(
        .framebuffer = fbo->fbo,
        .width = fbo->w,
        .height = fbo->h,
        .iformat = fbo->internal_format
    ));

    if (!tex) {
        MP_ERR(ctx, "Failed to wrap provided FBO as a libplacebo texture.\n");
        return MPV_ERROR_GENERIC;
    }

    *out_tex = tex;
    return 0;
}

/**
 * @brief Callback to mark the end of a frame rendering.
 * @param ctx The libmpv_gpu_next_context.
 */
static void libmpv_gpu_next_done_frame_gl(struct libmpv_gpu_next_context *ctx)
{
    // Nothing to do (yet), leaving the function empty.
}

/**
 * @brief Destroys the OpenGL context for the GPU next renderer.
 * @param ctx The libmpv_gpu_next_context to destroy.
 */
static void libmpv_gpu_next_destroy_gl(struct libmpv_gpu_next_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    // Don't use ra_free() here — destroy_ra_pl() already calls talloc_free(ra),
    // so ra_free() would double-free.
    talloc_free(ctx->old_ra);
    ctx->old_ra = NULL;

    if (p->ra) {
        ra_pl_destroy(&p->ra);
    }

    pl_opengl_destroy(&p->gl);
    pl_log_destroy(&p->pl_log);
}

/**
 * @brief Context functions for the OpenGL GPU next renderer.
 */
const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_gl = {
    .api_name = MPV_RENDER_API_TYPE_OPENGL,
    .init = libmpv_gpu_next_init_gl,
    .wrap_fbo = libmpv_gpu_next_wrap_fbo_gl,
    .done_frame = libmpv_gpu_next_done_frame_gl,
    .destroy = libmpv_gpu_next_destroy_gl,
};
#endif

#if HAVE_VULKAN && defined(PL_HAVE_VULKAN)
// Store Libplacebo Vulkan context information.
struct priv_vk {
    pl_log pl_log;
    pl_vulkan vulkan;
    pl_gpu gpu;
    struct ra_next *ra;
    pl_swapchain swapchain;  // Created when MPV_RENDER_PARAM_VULKAN_SURFACE is provided

    // Vulkan context wrapper exposed to hwdec drivers via ra_vk_ctx_get().
    // Populated after pl_vulkan_import() so hwdec can do zero-copy decode.
    struct mpvk_ctx mpvk;

    // Internal semaphore used for the hold/release cycle when the embedder
    // does not provide one. Created once at init, reused every frame.
    VkSemaphore internal_sem;

    // Per-frame state for the FBO path. Populated by wrap_fbo, consumed by
    // done_frame to complete the hold/release ownership cycle.
    pl_tex current_tex;              // wrapped texture for this frame
    VkImageLayout target_layout;     // layout to transition to after render
    mpv_vulkan_sync current_sync;    // semaphores for this frame (zeroed = none)
};

/**
 * @brief Callback to log messages from libplacebo (Vulkan).
 */
static void pl_log_cb_vk(void *log_priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = log_priv;
    mp_msg(log, MSGL_WARN, "[gpu-next:pl] %s\n", msg);
}

/**
 * @brief Initializes the Vulkan context for the GPU next renderer.
 * @param ctx The libmpv_gpu_next_context to initialize.
 * @param params The render parameters.
 * @return 0 on success, negative error code on failure.
 */
static int libmpv_gpu_next_init_vk(struct libmpv_gpu_next_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv_vk);
    struct priv_vk *p = ctx->priv;

    mpv_vulkan_init_params *vk_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!vk_params)
        return MPV_ERROR_INVALID_PARAMETER;

    if (!vk_params->instance || !vk_params->physical_device ||
        !vk_params->device || !vk_params->graphics_queue) {
        MP_ERR(ctx, "Missing required Vulkan handles\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Setup libplacebo logging — always enabled for diagnostics
    struct pl_log_params log_params = {
        .log_cb = pl_log_cb_vk,
        .log_priv = ctx->log,
        .log_level = PL_LOG_INFO,
    };

    p->pl_log = pl_log_create(PL_API_VER, &log_params);
    if (!p->pl_log) {
        MP_ERR(ctx, "Failed to create libplacebo log\n");
        return MPV_ERROR_GENERIC;
    }

    // Import user's Vulkan device into libplacebo
    PFN_vkGetInstanceProcAddr get_proc = vk_params->get_instance_proc_addr;
    if (!get_proc)
        get_proc = vkGetInstanceProcAddr;

    struct pl_vulkan_import_params import_params = {
        .instance = vk_params->instance,
        .phys_device = vk_params->physical_device,
        .device = vk_params->device,
        .get_proc_addr = get_proc,
        .queue_graphics = {
            .index = vk_params->graphics_queue_family,
            .count = 1,
        },
        .features = vk_params->features,
        .extensions = vk_params->extensions,
        .num_extensions = vk_params->num_extensions,
        .lock_queue = vk_params->lock_queue,
        .unlock_queue = vk_params->unlock_queue,
        .queue_ctx = vk_params->queue_ctx,
    };

    p->vulkan = pl_vulkan_import(p->pl_log, &import_params);
    if (!p->vulkan) {
        MP_ERR(ctx, "Failed to import Vulkan device\n");
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_UNSUPPORTED;
    }

    p->gpu = p->vulkan->gpu;

    // If a VkSurface was provided, create a libplacebo swapchain on it.
    // This makes the rendering path identical to standalone mpv — libplacebo
    // handles swapchain format selection, color management, and display profile.
    VkSurfaceKHR *surface_ptr =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_SURFACE, NULL);
    if (surface_ptr && *surface_ptr) {
        struct pl_vulkan_swapchain_params sw_params = {0};
        sw_params.surface = *surface_ptr;
        sw_params.present_mode = VK_PRESENT_MODE_FIFO_KHR;
        p->swapchain = pl_vulkan_create_swapchain(p->vulkan, &sw_params);
        if (p->swapchain) {
            // Hint HDR with display profile from host application
            struct pl_color_space hdr_hint = {
                .primaries = PL_COLOR_PRIM_BT_2020,
                .transfer = PL_COLOR_TRC_PQ,
            };
            mpv_display_profile *dp =
                get_mpv_render_param(params, MPV_RENDER_PARAM_DISPLAY_PROFILE, NULL);
            if (dp && dp->max_luma > 0 && dp->ref_luma > 0) {
                // Scale from display's reference white to libplacebo's
                // PL_COLOR_SDR_WHITE (203). Matches wayland_common.c:info_done().
                float a = dp->min_luma;
                float b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (dp->ref_luma - a);
                hdr_hint.hdr.min_luma = (dp->min_luma - a) * b + PL_COLOR_HDR_BLACK;
                hdr_hint.hdr.max_luma = (dp->max_luma - a) * b + PL_COLOR_HDR_BLACK;
                hdr_hint.hdr.max_cll = hdr_hint.hdr.max_luma;
                hdr_hint.hdr.max_fall = hdr_hint.hdr.max_luma;
                if (hdr_hint.hdr.min_luma < 0)
                    hdr_hint.hdr.min_luma = 0;
                MP_INFO(ctx, "Display: max=%.0f ref=%.0f -> scaled peak=%.0f min=%.4f\n",
                        dp->max_luma, dp->ref_luma, hdr_hint.hdr.max_luma, hdr_hint.hdr.min_luma);
            }
            pl_swapchain_colorspace_hint(p->swapchain, &hdr_hint);

            // Initial resize (0 = use surface's current extent)
            int w = 0, h = 0;
            pl_swapchain_resize(p->swapchain, &w, &h);

            // Probe actual format by doing a test frame
            struct pl_swapchain_frame test_frame;
            if (pl_swapchain_start_frame(p->swapchain, &test_frame)) {
                VkFormat fmt = VK_FORMAT_UNDEFINED;
                pl_vulkan_unwrap(p->gpu, test_frame.fbo, &fmt, NULL);
                MP_INFO(ctx, "Created libplacebo swapchain on provided VkSurface (%dx%d)\n", w, h);
                MP_INFO(ctx, "Swapchain format: VkFormat=%d, trc=%d, prim=%d, peak=%.0f\n",
                        fmt, test_frame.color_space.transfer, test_frame.color_space.primaries,
                        test_frame.color_space.hdr.max_luma);
                pl_swapchain_submit_frame(p->swapchain);
            } else {
                MP_INFO(ctx, "Created libplacebo swapchain on provided VkSurface (%dx%d)\n", w, h);
            }
        } else {
            MP_WARN(ctx, "Failed to create swapchain on VkSurface, falling back to FBO mode\n");
        }
    }

    // Create an internal binary semaphore for the hold/release fallback path
    // (when the embedder does not provide semaphores).
    p->internal_sem = pl_vulkan_sem_create(p->gpu, pl_vulkan_sem_params(
        .type = VK_SEMAPHORE_TYPE_BINARY,
    ));
    if (!p->internal_sem) {
        MP_ERR(ctx, "Failed to create internal semaphore\n");
        if (p->swapchain)
            pl_swapchain_destroy(&p->swapchain);
        pl_vulkan_destroy(&p->vulkan);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    // Create the RA abstraction
    p->ra = ra_pl_create(p->gpu, ctx->log, p->pl_log);
    if (!p->ra) {
        MP_ERR(ctx, "Failed to create RA from pl_gpu\n");
        pl_vulkan_sem_destroy(p->gpu, &p->internal_sem);
        if (p->swapchain)
            pl_swapchain_destroy(&p->swapchain);
        pl_vulkan_destroy(&p->vulkan);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    // Create old-style RA for hwdec infrastructure (wraps same pl_gpu)
    ctx->old_ra = ra_create_pl(p->gpu, mp_log_new(ctx, ctx->log, "old-ra"));
    if (!ctx->old_ra) {
        MP_ERR(ctx, "Failed to create old RA for hwdec\n");
        ra_pl_destroy(&p->ra);
        pl_vulkan_sem_destroy(p->gpu, &p->internal_sem);
        if (p->swapchain)
            pl_swapchain_destroy(&p->swapchain);
        pl_vulkan_destroy(&p->vulkan);
        pl_log_destroy(&p->pl_log);
        return MPV_ERROR_VO_INIT_FAILED;
    }

    // Expose Vulkan context to hwdec drivers.  ra_vk_ctx_get() checks for
    // a native resource named "mpvk_ctx" when the standard swapchain path
    // is unavailable (libmpv FBO render mode has no swapchain).
    p->mpvk = (struct mpvk_ctx) {
        .pllog  = p->pl_log,
        .vulkan = p->vulkan,
        .gpu    = p->gpu,
        // vkinst is NULL — pl_vulkan already exposes instance, get_proc_addr
        // Full device extension list from the embedder (pl_vulkan filters
        // these to only what libplacebo uses, but hwdec needs the full set
        // for FFmpeg's AVVulkanDeviceContext).
        .dev_extensions     = vk_params->extensions,
        .num_dev_extensions = vk_params->num_extensions,
    };
    ra_add_native_resource(ctx->old_ra, "mpvk_ctx", &p->mpvk);

    ctx->ra = p->ra;
    ctx->gpu = p->gpu;
    ctx->swapchain = p->swapchain;  // NULL if no surface was provided
    return 0;
}

/**
 * @brief Wraps a Vulkan image as a libplacebo texture.
 * @param ctx The libmpv_gpu_next_context.
 * @param params The render parameters.
 * @param out_tex Pointer to the output texture.
 * @return 0 on success, negative error code on failure.
 */
static int libmpv_gpu_next_wrap_fbo_vk(struct libmpv_gpu_next_context *ctx,
                    mpv_render_param *params, pl_tex *out_tex)
{
    struct priv_vk *p = ctx->priv;
    *out_tex = NULL;

    // Guard: if wrap_fbo is called again before done_frame completed the
    // previous hold/release cycle, the previous image's ownership is leaked.
    if (p->current_tex)
        MP_WARN(ctx, "wrap_fbo called before done_frame — previous frame not held\n");

    p->current_tex = NULL;
    p->current_sync = (mpv_vulkan_sync){0};

    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    if (!fbo->image || !fbo->width || !fbo->height) {
        MP_ERR(ctx, "Invalid Vulkan FBO parameters\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Read sync parameters (optional — if absent, done_frame falls back to
    // pl_gpu_finish which is correct but causes a full GPU stall).
    mpv_vulkan_sync *sync =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_SYNC, NULL);

    VkImageUsageFlags usage = fbo->usage;
    if (!usage)
        usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Wrap the embedder's VkImage. The wrapper is a lightweight CPU-side
    // descriptor — no pixel copies. The host destroys it via
    // ra_next_tex_destroy after done_frame completes the hold cycle, so
    // no previous wrapper for this image exists at this point.
    struct pl_vulkan_wrap_params wrap_params = {
        .image = fbo->image,
        .width = fbo->width,
        .height = fbo->height,
        .format = fbo->format,
        .usage = usage,
    };

    pl_tex tex = pl_vulkan_wrap(p->gpu, &wrap_params);
    if (!tex) {
        MP_ERR(ctx, "Failed to wrap VkImage as pl_tex\n");
        return MPV_ERROR_GENERIC;
    }

    // Release the texture to libplacebo — it starts out "held" by the user
    // after pl_vulkan_wrap(). If the embedder provided a wait semaphore,
    // libplacebo will wait on it before touching the image.
    struct pl_vulkan_release_params release_params = {
        .tex = tex,
        .layout = fbo->current_layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    };
    if (sync && sync->wait_semaphore) {
        release_params.semaphore = (pl_vulkan_sem) {
            .sem = sync->wait_semaphore,
            .value = sync->wait_value,
        };
    }
    pl_vulkan_release_ex(p->gpu, &release_params);

    // Stash per-frame state for done_frame to complete the ownership cycle.
    p->current_tex = tex;
    p->target_layout = fbo->target_layout;
    if (sync)
        p->current_sync = *sync;

    *out_tex = tex;
    return 0;
}

/**
 * @brief Callback to mark the end of a frame rendering (Vulkan).
 *
 * Transfers texture ownership back to the embedder via pl_vulkan_hold_ex.
 * If the embedder provided a signal semaphore via MPV_RENDER_PARAM_VULKAN_SYNC,
 * that semaphore is signaled when the image is ready. Otherwise an internal
 * semaphore is used and we stall with pl_gpu_finish() so the image is safe
 * for the embedder to use immediately after this call returns.
 */
static void libmpv_gpu_next_done_frame_vk(struct libmpv_gpu_next_context *ctx)
{
    struct priv_vk *p = ctx->priv;

    if (!p->current_tex) {
        // Swapchain path or error — nothing to hold.
        return;
    }

    VkSemaphore sem;
    uint64_t sem_value;
    bool user_provided_sem;

    if (p->current_sync.signal_semaphore) {
        sem = p->current_sync.signal_semaphore;
        sem_value = p->current_sync.signal_value;
        user_provided_sem = true;
    } else if (p->internal_sem) {
        sem = p->internal_sem;
        sem_value = 0;
        user_provided_sem = false;
    } else {
        // Internal semaphore lost (recreation failed on prior frame).
        // Stall to ensure safety.
        pl_gpu_finish(p->gpu);
        p->current_tex = NULL;
        return;
    }

    // Always hold — this transfers ownership back to the embedder and
    // transitions the image to the requested layout.
    bool ok = pl_vulkan_hold_ex(p->gpu, pl_vulkan_hold_params(
        .tex = p->current_tex,
        .layout = p->target_layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = (pl_vulkan_sem) {
            .sem = sem,
            .value = sem_value,
        },
    ));
    if (!ok) {
        // Hold failed — the signal semaphore was never signaled. Stall the
        // GPU so the embedder at least gets a quiescent image rather than
        // deadlocking on a semaphore that will never fire.
        MP_ERR(ctx, "pl_vulkan_hold_ex failed, falling back to gpu_finish\n");
        pl_gpu_finish(p->gpu);

        // The internal binary semaphore is now in an unknown state — it was
        // never signaled, so the next frame's hold would deadlock. Recreate it.
        if (!user_provided_sem) {
            pl_vulkan_sem_destroy(p->gpu, &p->internal_sem);
            p->internal_sem = pl_vulkan_sem_create(p->gpu, pl_vulkan_sem_params(
                .type = VK_SEMAPHORE_TYPE_BINARY,
            ));
            if (!p->internal_sem)
                MP_ERR(ctx, "Failed to recreate internal semaphore\n");
        }
    } else if (!user_provided_sem) {
        // No user semaphore — stall to ensure the image is safe for the
        // embedder immediately after this call returns.
        pl_gpu_finish(p->gpu);
    }

    p->current_tex = NULL;
}

/**
 * @brief Destroys the Vulkan context for the GPU next renderer.
 * @param ctx The libmpv_gpu_next_context to destroy.
 */
static void libmpv_gpu_next_destroy_vk(struct libmpv_gpu_next_context *ctx)
{
    struct priv_vk *p = ctx->priv;
    if (!p)
        return;

    // Handle mid-frame teardown: if a texture is still in flight, complete
    // the ownership cycle before tearing down resources.
    if (p->current_tex)
        libmpv_gpu_next_done_frame_vk(ctx);

    // Don't use ra_free() here — destroy_ra_pl() already calls talloc_free(ra),
    // so ra_free() would double-free.
    talloc_free(ctx->old_ra);
    ctx->old_ra = NULL;

    if (p->ra) {
        ra_pl_destroy(&p->ra);
    }

    if (p->internal_sem) {
        pl_vulkan_sem_destroy(p->gpu, &p->internal_sem);
    }

    if (p->swapchain) {
        pl_swapchain_destroy(&p->swapchain);
    }

    if (p->vulkan) {
        pl_vulkan_destroy(&p->vulkan);
    }

    pl_log_destroy(&p->pl_log);
}

/**
 * @brief Context functions for the Vulkan GPU next renderer.
 */
const struct libmpv_gpu_next_context_fns libmpv_gpu_next_context_vk = {
    .api_name = MPV_RENDER_API_TYPE_VULKAN,
    .init = libmpv_gpu_next_init_vk,
    .wrap_fbo = libmpv_gpu_next_wrap_fbo_vk,
    .done_frame = libmpv_gpu_next_done_frame_vk,
    .destroy = libmpv_gpu_next_destroy_vk,
};
#endif

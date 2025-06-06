/**
* AUTO-GENERATED - DO NOT EDIT. Source: https://github.com/gpuweb/cts
**/export const description = `
API validation tests for dynamic state commands (setViewport/ScissorRect/BlendColor...).

TODO: ensure existing tests cover these notes. Note many of these may be operation tests instead.
> - setViewport
>     - {x, y} = {0, invalid values if any}
>     - {width, height, minDepth, maxDepth} = {
>         - least possible value that's valid
>         - greatest possible negative value that's invalid
>         - greatest possible positive value that's valid
>         - least possible positive value that's invalid if any
>         - }
>     - minDepth {<, =, >} maxDepth
> - setScissorRect
>     - {width, height} = 0
>     - {x+width, y+height} = attachment size + 1
> - setBlendConstant
>     - color {slightly, very} out of range
>     - used with a simple pipeline that {does, doesn't} use it
> - setStencilReference
>     - {0, max}
>     - used with a simple pipeline that {does, doesn't} use it
`;import { makeTestGroup } from '../../../../../../common/framework/test_group.js';
import { AllFeaturesMaxLimitsGPUTest } from '../../../../../gpu_test.js';
import { nextAfterF32 } from '../../../../../util/math.js';

















class F extends AllFeaturesMaxLimitsGPUTest {
  testViewportCall(
  success,
  v,
  attachmentSize = { width: 1, height: 1, depthOrArrayLayers: 1 })
  {
    const attachment = this.createTextureTracked({
      format: 'rgba8unorm',
      size: attachmentSize,
      usage: GPUTextureUsage.RENDER_ATTACHMENT
    });

    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [
      {
        view: attachment.createView(),
        loadOp: 'load',
        storeOp: 'store'
      }]

    });
    pass.setViewport(v.x, v.y, v.w, v.h, v.minDepth, v.maxDepth);
    pass.end();

    this.expectValidationError(() => {
      encoder.finish();
    }, !success);
  }

  testScissorCall(
  success,
  s,
  attachmentSize = { width: 1, height: 1, depthOrArrayLayers: 1 })
  {
    const attachment = this.createTextureTracked({
      format: 'rgba8unorm',
      size: attachmentSize,
      usage: GPUTextureUsage.RENDER_ATTACHMENT
    });

    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [
      {
        view: attachment.createView(),
        loadOp: 'load',
        storeOp: 'store'
      }]

    });
    if (success === 'type-error') {
      this.shouldThrow('TypeError', () => {
        pass.setScissorRect(s.x, s.y, s.w, s.h);
      });
    } else {
      pass.setScissorRect(s.x, s.y, s.w, s.h);
      pass.end();

      this.expectValidationError(() => {
        encoder.finish();
      }, !success);
    }
  }

  createDummyRenderPassEncoder() {
    const attachment = this.createTextureTracked({
      format: 'rgba8unorm',
      size: [1, 1, 1],
      usage: GPUTextureUsage.RENDER_ATTACHMENT
    });

    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [
      {
        view: attachment.createView(),
        loadOp: 'load',
        storeOp: 'store'
      }]

    });

    return { encoder, pass };
  }
}

export const g = makeTestGroup(F);

g.test('setViewport,width_height_nonnegative').
desc(
  `Test that the width and height parameters of setViewport must be non-negative.

TODO Test -0 (it should be valid) but can't be tested because the harness complains about duplicate parameters.
TODO Test the first value smaller than -0`
).
paramsSubcasesOnly([
// Control case: everything to 0 is ok, covers the empty viewport case.
{ x: 0, y: 0, w: 0, h: 0 },

// Negative width/height is invalid
{ x: 0, y: 0, w: -1, h: 0 },
{ x: 0, y: 0, w: 0, h: -1 },

// Negative width/height is invalid even if the resulting bounds are positive
{ x: 1, y: 0, w: -1, h: 0 },
{ x: 0, y: 1, w: 0, h: -1 }]
).
fn((t) => {
  const { x, y, w, h } = t.params;
  const success = w >= 0 && h >= 0;
  t.testViewportCall(success, { x, y, w, h, minDepth: 0, maxDepth: 1 });
});

g.test('setViewport,exceeds_attachment_size').
desc(`Test that the viewport can exceed the attachment size`).
paramsSubcasesOnly([
{ attachmentWidth: 3, attachmentHeight: 3 },
{ attachmentWidth: 1024, attachmentHeight: 1024 }]
).
fn((t) => {
  const { attachmentWidth, attachmentHeight } = t.params;
  t.testViewportCall(
    true,
    { x: 0, y: 0, w: attachmentWidth + 1, h: attachmentHeight + 1, minDepth: 0, maxDepth: 1 },
    { width: attachmentWidth, height: attachmentHeight, depthOrArrayLayers: 1 }
  );
});

g.test('setViewport,xy_rect_contained_in_bounds').
desc(
  `Test that the rectangle defined by x, y, width, height must be contained in the maximum viewport bounds
and that the viewport size cannot exceed the maximum.`
).
paramsSubcasesOnly((u) =>
u.combine('dimension', [0, 1]).combineWithParams([
// Control case: max viewport is valid.
{ om: 0, od: 0, sd: 0, _success: true },

// Other valid cases
{ om: -1, od: 0, sd: 0, _success: true },
{ om: -2, od: 0, sd: 0, _success: true },
{ om: 1, od: -1, sd: 0, _success: true },
{ om: 0, od: -1, sd: 0, _success: true },
{ om: 0, od: 1, sd: 0, _success: true },
{ om: 1, od: 0, sd: -1, _success: true },

// Cases that go outside the allowed bounds
{ om: -2, od: -1, sd: 0, _success: false },
{ om: 1, od: 0, sd: 0, _success: false },
{ om: 1, od: 1, sd: -1, _success: false },
{ om: 1, od: 'negative', sd: 0, _success: false },

// Case that exceeds the max viewport size
{ om: 0, od: 0, sd: 1, _success: false },
{ om: 0, od: 0, sd: 'positive', _success: false }]
)
).
fn((t) => {
  const { dimension, om, od, sd, _success } = t.params;

  const maxViewportSize = t.device.limits.maxTextureDimension2D;

  const xy = [0, 0];
  const wh = [maxViewportSize, maxViewportSize];

  xy[dimension] = maxViewportSize * om;

  if (od === 'negative' || od === 'positive') {
    xy[dimension] = nextAfterF32(xy[dimension], od, 'no-flush');
  } else {
    xy[dimension] += od;
  }

  if (sd === 'negative' || sd === 'positive') {
    wh[dimension] = nextAfterF32(wh[dimension], sd, 'no-flush');
  } else {
    wh[dimension] += sd;
  }

  const x = xy[0];
  const y = xy[1];
  const w = wh[0];
  const h = wh[1];

  t.testViewportCall(_success, { x, y, w, h, minDepth: 0, maxDepth: 1 });
});

g.test('setViewport,depth_rangeAndOrder').
desc('Test that 0 <= minDepth <= maxDepth <= 1').
paramsSubcasesOnly([
// Success cases
{ minDepth: 0, maxDepth: 1 },
{ minDepth: -0, maxDepth: -0 },
{ minDepth: 1, maxDepth: 1 },
{ minDepth: 0.3, maxDepth: 0.7 },
{ minDepth: 0.7, maxDepth: 0.7 },
{ minDepth: 0.3, maxDepth: 0.3 },

// Invalid cases
{ minDepth: -0.1, maxDepth: 1 },
{ minDepth: 0, maxDepth: 1.1 },
{ minDepth: 0.5, maxDepth: 0.49999 }]
).
fn((t) => {
  const { minDepth, maxDepth } = t.params;
  const success =
  0 <= minDepth && minDepth <= 1 && 0 <= maxDepth && maxDepth <= 1 && minDepth <= maxDepth;
  t.testViewportCall(success, { x: 0, y: 0, w: 1, h: 1, minDepth, maxDepth });
});

g.test('setScissorRect,x_y_width_height_nonnegative').
desc(
  `Test that the parameters of setScissorRect to define the box must be non-negative or a TypeError is thrown.

TODO Test -0 (it should be valid) but can't be tested because the harness complains about duplicate parameters.
TODO Test the first value smaller than -0`
).
paramsSubcasesOnly([
// Control case: everything to 0 is ok, covers the empty scissor case.
{ x: 0, y: 0, w: 0, h: 0 },

// Test -1
{ x: -1, y: 0, w: 0, h: 0 },
{ x: 0, y: -1, w: 0, h: 0 },
{ x: 0, y: 0, w: -1, h: 0 },
{ x: 0, y: 0, w: 0, h: -1 }]
).
fn((t) => {
  const { x, y, w, h } = t.params;
  const success = x >= 0 && y >= 0 && w >= 0 && h >= 0;
  t.testScissorCall(success ? true : 'type-error', { x, y, w, h });
});

g.test('setScissorRect,xy_rect_contained_in_attachment').
desc(
  'Test that the rectangle defined by x, y, width, height must be contained in the attachments'
).
paramsSubcasesOnly((u) =>
u.
combineWithParams([
{ attachmentWidth: 3, attachmentHeight: 5 },
{ attachmentWidth: 5, attachmentHeight: 3 },
{ attachmentWidth: 1024, attachmentHeight: 1 },
{ attachmentWidth: 1, attachmentHeight: 1024 }]
).
combineWithParams([
// Control case: a full scissor is valid.
{ dx: 0, dy: 0, dw: 0, dh: 0 },

// Other valid cases with a partial scissor.
{ dx: 1, dy: 0, dw: -1, dh: 0 },
{ dx: 0, dy: 1, dw: 0, dh: -1 },
{ dx: 0, dy: 0, dw: -1, dh: 0 },
{ dx: 0, dy: 0, dw: 0, dh: -1 },

// Test with a small value that causes the scissor to go outside the attachment.
{ dx: 1, dy: 0, dw: 0, dh: 0 },
{ dx: 0, dy: 1, dw: 0, dh: 0 },
{ dx: 0, dy: 0, dw: 1, dh: 0 },
{ dx: 0, dy: 0, dw: 0, dh: 1 }]
)
).
fn((t) => {
  const { attachmentWidth, attachmentHeight, dx, dy, dw, dh } = t.params;
  const x = dx;
  const y = dy;
  const w = attachmentWidth + dw;
  const h = attachmentWidth + dh;

  const success = x + w <= attachmentWidth && y + h <= attachmentHeight;
  t.testScissorCall(
    success,
    { x, y, w, h },
    { width: attachmentWidth, height: attachmentHeight, depthOrArrayLayers: 1 }
  );
});

g.test('setBlendConstant').
desc('Test that almost any color value is valid for setBlendConstant').
paramsSubcasesOnly([
{ r: 1.0, g: 1.0, b: 1.0, a: 1.0 },
{ r: -1.0, g: -1.0, b: -1.0, a: -1.0 },
{ r: Number.MAX_SAFE_INTEGER, g: Number.MIN_SAFE_INTEGER, b: -0, a: 100000 }]
).
fn((t) => {
  const { r, g, b, a } = t.params;
  const encoders = t.createDummyRenderPassEncoder();
  encoders.pass.setBlendConstant({ r, g, b, a });
  encoders.pass.end();
  encoders.encoder.finish();
});

g.test('setStencilReference').
desc('Test that almost any stencil reference value is valid for setStencilReference').
paramsSubcasesOnly([
{ value: 1 }, //
{ value: 0 },
{ value: 1000 },
{ value: 0xffffffff }]
).
fn((t) => {
  const { value } = t.params;
  const encoders = t.createDummyRenderPassEncoder();
  encoders.pass.setStencilReference(value);
  encoders.pass.end();
  encoders.encoder.finish();
});
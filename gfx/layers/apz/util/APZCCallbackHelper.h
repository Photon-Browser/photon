/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_APZCCallbackHelper_h
#define mozilla_layers_APZCCallbackHelper_h

#include "InputData.h"
#include "LayersTypes.h"
#include "Units.h"
#include "mozilla/EventForwards.h"
#include "mozilla/layers/MatrixMessage.h"
#include "nsRefreshObservers.h"

#include <functional>

class nsIContent;
class nsIWidget;
class nsPresContext;
template <class T>
struct already_AddRefed;
template <class T>
class nsCOMPtr;

namespace mozilla {

enum class PreventDefaultResult : uint8_t { No, ByContent, ByChrome };

class PresShell;
class ScrollContainerFrame;
enum class PreventDefaultResult : uint8_t;

namespace layers {

struct RepaintRequest;

namespace apz {
enum class PrecedingPointerDown : bool;
}

enum class SynthesizeForTests : bool { No, Yes };

/* Refer to documentation on SendSetTargetAPZCNotification for this class */
class DisplayportSetListener : public ManagedPostRefreshObserver {
 public:
  DisplayportSetListener(nsIWidget* aWidget, nsPresContext*,
                         const uint64_t& aInputBlockId,
                         nsTArray<ScrollableLayerGuid>&& aTargets);
  virtual ~DisplayportSetListener();
  void Register();

 private:
  RefPtr<nsIWidget> mWidget;
  uint64_t mInputBlockId;
  nsTArray<ScrollableLayerGuid> mTargets;

  void OnPostRefresh();
};

/* This class contains some helper methods that facilitate implementing the
   GeckoContentController callback interface required by the
   AsyncPanZoomController. Since different platforms need to implement this
   interface in similar-but- not-quite-the-same ways, this utility class
   provides some helpful methods to hold code that can be shared across the
   different platform implementations.
 */
class APZCCallbackHelper {
  typedef mozilla::layers::ScrollableLayerGuid ScrollableLayerGuid;

 public:
  using PrecedingPointerDown = apz::PrecedingPointerDown;

  static void NotifyLayerTransforms(const nsTArray<MatrixMessage>& aTransforms);

  /* Applies the scroll and zoom parameters from the given RepaintRequest object
     to the root frame for the given metrics' scrollId. If tiled thebes layers
     are enabled, this will align the displayport to tile boundaries. Setting
     the scroll position can cause some small adjustments to be made to the
     actual scroll position. */
  static void UpdateRootFrame(const RepaintRequest& aRequest);

  /* Applies the scroll parameters from the given RepaintRequest object to the
     subframe corresponding to given metrics' scrollId. If tiled thebes
     layers are enabled, this will align the displayport to tile boundaries.
     Setting the scroll position can cause some small adjustments to be made
     to the actual scroll position. */
  static void UpdateSubFrame(const RepaintRequest& aRequest);

  /* Get the presShellId and view ID for the given content element.
   * If the view ID does not exist, one is created.
   * The pres shell ID should generally already exist; if it doesn't for some
   * reason, false is returned. */
  static bool GetOrCreateScrollIdentifiers(
      nsIContent* aContent, uint32_t* aPresShellIdOut,
      ScrollableLayerGuid::ViewID* aViewIdOut);

  /* Initialize a zero-margin displayport on the root document element of the
     given presShell. */
  static void InitializeRootDisplayport(PresShell* aPresShell);

  /* Similar to above InitializeRootDisplayport but for an nsIFrame.
     The nsIFrame needs to be a popup menu frame. */
  static void InitializeRootDisplayport(nsIFrame* aFrame);

  /* Get the pres context associated with the document enclosing |aContent|. */
  static nsPresContext* GetPresContextForContent(nsIContent* aContent);

  /* Get the pres shell associated with the root content document enclosing
   * |aContent|. */
  static PresShell* GetRootContentDocumentPresShellForContent(
      nsIContent* aContent);

  /* Dispatch a widget event via the widget stored in the event, if any.
   * In a child process, allows the BrowserParent event-capture mechanism to
   * intercept the event. */
  static nsEventStatus DispatchWidgetEvent(WidgetGUIEvent& aEvent);

  /* Synthesize a mouse event with the given parameters, and dispatch it
   * via the given widget. */
  MOZ_CAN_RUN_SCRIPT static nsEventStatus DispatchSynthesizedMouseEvent(
      EventMessage aMsg, const LayoutDevicePoint& aRefPoint,
      uint32_t aPointerId, Modifiers aModifiers, int32_t aClickCount,
      PrecedingPointerDown aPrecedingPointerDownState, nsIWidget* aWidget,
      SynthesizeForTests aSynthesizeForTests);

  /*
   * Synthesize a contextmenu event with the given parameters, and dispatch it
   * via the given widget.
   */
  MOZ_CAN_RUN_SCRIPT static PreventDefaultResult
  DispatchSynthesizedContextmenuEvent(const LayoutDevicePoint& aRefPoint,
                                      uint32_t aPointerId, Modifiers aModifiers,
                                      nsIWidget* aWidget,
                                      SynthesizeForTests aSynthesizeForTests);

  /* Fire a single-tap event at the given point. The event is dispatched
   * via the given widget. */
  MOZ_CAN_RUN_SCRIPT static void FireSingleTapEvent(
      const LayoutDevicePoint& aPoint, uint32_t aPointerId,
      Modifiers aModifiers, int32_t aClickCount,
      PrecedingPointerDown aPrecedingPointerDownState, nsIWidget* aWidget,
      SynthesizeForTests aSynthesizeForTests);

  /* Perform hit-testing on the touch points of |aEvent| to determine
   * which scrollable frames they target. If any of these frames don't have
   * a displayport, set one.
   *
   * If any displayports need to be set, this function returns a heap-allocated
   * object. The caller is responsible for calling Register() on that object.
   *
   * The object registers itself as a post-refresh observer on the presShell
   * and ensures that notifications get sent to APZ correctly after the
   * refresh.
   *
   * Having the caller manage this object is desirable in case they want to
   * (a) know about the fact that a displayport needs to be set, and
   * (b) register a post-refresh observer of their own that will run in
   *     a defined ordering relative to the APZ messages.
   */
  static already_AddRefed<DisplayportSetListener> SendSetTargetAPZCNotification(
      nsIWidget* aWidget, mozilla::dom::Document* aDocument,
      const WidgetGUIEvent& aEvent, const LayersId& aLayersId,
      uint64_t aInputBlockId);

  /* Notify content of a mouse scroll testing event. */
  static void NotifyMozMouseScrollEvent(
      const ScrollableLayerGuid::ViewID& aScrollId, const nsString& aEvent);

  /* Notify content that the repaint flush is complete. */
  static void NotifyFlushComplete(PresShell* aPresShell);

  static void NotifyAsyncScrollbarDragInitiated(
      uint64_t aDragBlockId, const ScrollableLayerGuid::ViewID& aScrollId,
      ScrollDirection aDirection);
  static void NotifyAsyncScrollbarDragRejected(
      const ScrollableLayerGuid::ViewID& aScrollId);
  static void NotifyAsyncAutoscrollRejected(
      const ScrollableLayerGuid::ViewID& aScrollId);

  static void CancelAutoscroll(const ScrollableLayerGuid::ViewID& aScrollId);
  static void NotifyScaleGestureComplete(const nsCOMPtr<nsIWidget>& aWidget,
                                         float aScale);

  /*
   * Check if the scroll container frame is currently in the middle of a main
   * thread async or smooth scroll, or has already requested some other apz
   * scroll that hasn't been acknowledged by apz.
   *
   * We want to discard apz updates to the main-thread scroll offset if this is
   * true to prevent clobbering higher priority origins.
   */
  static bool IsScrollInProgress(ScrollContainerFrame* aFrame);

  /* Notify content of the progress of a pinch gesture that APZ won't do
   * zooming for (because the apz.allow_zooming pref is false). This function
   * will dispatch appropriate WidgetSimpleGestureEvent events to gecko.
   */
  static void NotifyPinchGesture(PinchGestureInput::PinchGestureType aType,
                                 const LayoutDevicePoint& aFocusPoint,
                                 LayoutDeviceCoord aSpanChange,
                                 Modifiers aModifiers,
                                 const nsCOMPtr<nsIWidget>& aWidget);

 private:
  static uint64_t sLastTargetAPZCNotificationInputBlock;
};

}  // namespace layers

std::ostream& operator<<(std::ostream& aOut,
                         const PreventDefaultResult aPreventDefaultResult);

}  // namespace mozilla

#endif /* mozilla_layers_APZCCallbackHelper_h */

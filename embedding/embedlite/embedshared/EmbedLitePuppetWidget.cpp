/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EmbedLog.h"

#include "base/basictypes.h"

#include "BasicLayers.h"
#include "gfxPlatform.h"
#if defined(MOZ_ENABLE_D3D10_LAYER)
# include "LayerManagerD3D10.h"
#endif
#include "mozilla/Hal.h"
#include "mozilla/layers/CompositorChild.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/ipc/MessageChannel.h"
#include "EmbedLitePuppetWidget.h"
#include "EmbedLiteView.h"
#include "nsIWidgetListener.h"

#include "Layers.h"
#include "BasicLayers.h"
#include "ClientLayerManager.h"
#include "GLContextProvider.h"
#include "GLContext.h"
#include "GLLibraryEGL.h"
#include "EmbedLiteCompositorParent.h"
#include "mozilla/Preferences.h"
#include "EmbedLiteApp.h"
#include "LayerScope.h"
#include "mozilla/unused.h"
#include "mozilla/BasicEvents.h"

using namespace mozilla::dom;
using namespace mozilla::gl;
using namespace mozilla::hal;
using namespace mozilla::layers;
using namespace mozilla::widget;
using namespace mozilla::ipc;

namespace mozilla {
namespace embedlite {

// Arbitrary, fungible.
const size_t EmbedLitePuppetWidget::kMaxDimension = 4000;

static nsTArray<EmbedLitePuppetWidget*> gTopLevelWindows;
static bool sFailedToCreateGLContext = false;
static bool sUseExternalGLContext = false;
static bool sRequestGLContextEarly = false;

NS_IMPL_ISUPPORTS_INHERITED(EmbedLitePuppetWidget, nsBaseWidget,
                            nsISupportsWeakReference)

static bool
IsPopup(const nsWidgetInitData* aInitData)
{
  return aInitData && aInitData->mWindowType == eWindowType_popup;
}

static void
InvalidateRegion(nsIWidget* aWidget, const nsIntRegion& aRegion)
{
  nsIntRegionRectIterator it(aRegion);
  while(const nsIntRect* r = it.Next()) {
    aWidget->Invalidate(*r);
  }
}

EmbedLitePuppetWidget*
EmbedLitePuppetWidget::TopWindow()
{
  if (!gTopLevelWindows.IsEmpty()) {
    return gTopLevelWindows[0];
  }
  return nullptr;
}

bool
EmbedLitePuppetWidget::IsTopLevel()
{
  return mWindowType == eWindowType_toplevel ||
         mWindowType == eWindowType_dialog ||
         mWindowType == eWindowType_invisible;
}

EmbedLitePuppetWidget::EmbedLitePuppetWidget(EmbedLiteViewChildIface* aEmbed, uint32_t& aId)
  : mEmbed(aEmbed)
  , mVisible(false)
  , mEnabled(false)
  , mIMEComposing(false)
  , mParent(nullptr)
  , mId(aId)
{
  MOZ_COUNT_CTOR(EmbedLitePuppetWidget);
  LOGT("this:%p", this);
  static bool prefsInitialized = false;
  if (!prefsInitialized) {
    Preferences::AddBoolVarCache(&sUseExternalGLContext,
        "embedlite.compositor.external_gl_context", false);
    Preferences::AddBoolVarCache(&sRequestGLContextEarly,
        "embedlite.compositor.request_external_gl_context_early", false);
    prefsInitialized = true;
  }
}

EmbedLitePuppetWidget::~EmbedLitePuppetWidget()
{
  MOZ_COUNT_DTOR(EmbedLitePuppetWidget);
  LOGT("this:%p", this);
  gTopLevelWindows.RemoveElement(this);
}

NS_IMETHODIMP
EmbedLitePuppetWidget::SetParent(nsIWidget* aParent)
{
  LOGT();
  mParent = aParent;
  return NS_OK;
}

nsIWidget*
EmbedLitePuppetWidget::GetParent(void)
{
  return mParent;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::Create(nsIWidget*        aParent,
                              nsNativeWidget   aNativeParent,
                              const nsIntRect&  aRect,
                              nsWidgetInitData* aInitData)
{
  LOGT();
  MOZ_ASSERT(!aNativeParent, "got a non-Puppet native parent");

  mParent = aParent;
  BaseCreate(aParent, aRect, aInitData);

  mBounds = aRect;
  mEnabled = true;
  mVisible = true;

  EmbedLitePuppetWidget* parent = static_cast<EmbedLitePuppetWidget*>(aParent);
  if (parent) {
    parent->mChild = this;
  } else {
    Resize(mBounds.x, mBounds.y, mBounds.width, mBounds.height, false);
  }

  if (IsTopLevel()) {
    LOGT("Append this to toplevel windows:%p", this);
    gTopLevelWindows.AppendElement(this);
  }

  if (sUseExternalGLContext && sRequestGLContextEarly) {
    // GetPlatform() should create compositor loop if it doesn't exist, yet.
    gfxPlatform::GetPlatform();
    CompositorParent::CompositorLoop()->PostTask(FROM_HERE,
        NewRunnableFunction(&CreateGLContextEarly, mId));
  }

  return NS_OK;
}

already_AddRefed<nsIWidget>
EmbedLitePuppetWidget::CreateChild(const nsIntRect&  aRect,
                                   nsWidgetInitData* aInitData,
                                   bool              aForceUseIWidgetParent)
{
  LOGT();
  bool isPopup = IsPopup(aInitData);
  nsCOMPtr<nsIWidget> widget = new EmbedLitePuppetWidget(mEmbed, mId);
  return ((widget &&
           NS_SUCCEEDED(widget->Create(isPopup ? nullptr: this, nullptr, aRect,
                                       aInitData))) ?
          widget.forget() : nullptr);
}

NS_IMETHODIMP
EmbedLitePuppetWidget::Destroy()
{
  LOGT();
  if (mOnDestroyCalled) {
    return NS_OK;
  }

  mOnDestroyCalled = true;

  nsIWidget* topWidget = GetTopLevelWidget();
  if (mLayerManager && topWidget == this) {
    mLayerManager->Destroy();
  }
  mLayerManager = nullptr;

  Base::OnDestroy();
  Base::Destroy();
  mParent = nullptr;
  mEmbed = nullptr;
  mChild = nullptr;

  DestroyCompositor();

  return NS_OK;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::Show(bool aState)
{
  LOGT("t:%p, state: %i, LM:%p", this, aState, mLayerManager.get());
  NS_ASSERTION(mEnabled,
               "does it make sense to Show()/Hide() a disabled widget?");

  bool wasVisible = mVisible;
  mVisible = aState;

  if (mChild) {
    mChild->mVisible = aState;
  }

  nsIWidget* topWidget = GetTopLevelWidget();
  if (!mVisible && mLayerManager && topWidget == this) {
    mLayerManager->ClearCachedResources();
  }

  if (!wasVisible && mVisible) {
    Resize(mBounds.width, mBounds.height, false);
    Invalidate(mBounds);
  }

  return NS_OK;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::Resize(double aWidth,
                              double aHeight,
                              bool    aRepaint)
{
  nsIntRect oldBounds = mBounds;
  LOGT("sz[%i,%i]->[%g,%g]", oldBounds.width, oldBounds.height, aWidth, aHeight);
  mBounds.SizeTo(nsIntSize(NSToIntRound(aWidth), NSToIntRound(aHeight)));
  nsIWidget* topWidget = GetTopLevelWidget();
  if (topWidget)
    static_cast<EmbedLitePuppetWidget*>(topWidget)->mBounds = mBounds;

  if (mChild) {
    return mChild->Resize(aWidth, aHeight, aRepaint);
  }

  // XXX: roc says that |aRepaint| dictates whether or not to
  // invalidate the expanded area
  if (oldBounds.Size() < mBounds.Size() && aRepaint) {
    nsIntRegion dirty(mBounds);
    dirty.Sub(dirty,  oldBounds);
    InvalidateRegion(this, dirty);
  }

  nsIWidgetListener* listener =
    mAttachedWidgetListener ? mAttachedWidgetListener : mWidgetListener;
  if (!oldBounds.IsEqualEdges(mBounds) && listener) {
    listener->WindowResized(this, mBounds.width, mBounds.height);
  }

  return NS_OK;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::SetFocus(bool aRaise)
{
  LOGT();
  return NS_OK;
}

void*
EmbedLitePuppetWidget::GetNativeData(uint32_t aDataType)
{
  LOGT("t:%p, DataType: %i", this, aDataType);
  switch (aDataType) {
    case NS_NATIVE_SHAREABLE_WINDOW: {
      LOGW("aDataType:%i\n", __LINE__, aDataType);
      return (void*)nullptr;
    }
    case NS_NATIVE_OPENGL_CONTEXT: {
      MOZ_ASSERT(!GetParent());
      return GetGLContext();
    }
    case NS_NATIVE_WINDOW:
    case NS_NATIVE_DISPLAY:
    case NS_NATIVE_PLUGIN_PORT:
    case NS_NATIVE_GRAPHIC:
    case NS_NATIVE_SHELLWIDGET:
    case NS_NATIVE_WIDGET:
      LOGW("nsWindow::GetNativeData not implemented for this type");
      break;
    default:
      NS_WARNING("nsWindow::GetNativeData called with bad value");
      break;
  }

  return nullptr;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::DispatchEvent(WidgetGUIEvent* event, nsEventStatus& aStatus)
{
  MOZ_ASSERT(!mChild || mChild->mWindowType == eWindowType_popup,
                    "Unexpected event dispatch!");

  aStatus = nsEventStatus_eIgnore;

  nsIWidgetListener* listener =
    mAttachedWidgetListener ? mAttachedWidgetListener : mWidgetListener;

  MOZ_ASSERT(listener, "No listener!");

  if (event->mClass == eKeyboardEventClass) {
    RemoveIMEComposition();
  }

  aStatus = listener->HandleEvent(event, mUseAttachedEvents);

  switch (event->message) {
    case NS_COMPOSITION_START:
      MOZ_ASSERT(!mIMEComposing);
      mIMEComposing = true;
      break;
    case NS_COMPOSITION_END:
      MOZ_ASSERT(mIMEComposing);
      mIMEComposing = false;
      mIMEComposingText.Truncate();
      break;
    case NS_COMPOSITION_CHANGE:
      MOZ_ASSERT(mIMEComposing);
      mIMEComposingText = static_cast<WidgetCompositionEvent*>(event)->mData;
      break;
  }

  return NS_OK;
}

NS_IMETHODIMP
EmbedLitePuppetWidget::ResetInputState()
{
  LOGT();
  RemoveIMEComposition();
  return NS_OK;
}


NS_IMETHODIMP_(void)
EmbedLitePuppetWidget::SetInputContext(const InputContext& aContext,
                                       const InputContextAction& aAction)
{
  LOGT("IME: SetInputContext: s=0x%X, 0x%X, action=0x%X, 0x%X",
       aContext.mIMEState.mEnabled, aContext.mIMEState.mOpen,
       aAction.mCause, aAction.mFocusChange);

  mInputContext = aContext;

  // Ensure that opening the virtual keyboard is allowed for this specific
  // InputContext depending on the content.ime.strict.policy pref
  if (aContext.mIMEState.mEnabled != IMEState::DISABLED &&
      aContext.mIMEState.mEnabled != IMEState::PLUGIN &&
      Preferences::GetBool("content.ime.strict_policy", false) &&
      !aAction.ContentGotFocusByTrustedCause() &&
      !aAction.UserMightRequestOpenVKB()) {
    return;
  }

  if (!mEmbed) {
    return;
  }

  mEmbed->SetInputContext(
    static_cast<int32_t>(aContext.mIMEState.mEnabled),
    static_cast<int32_t>(aContext.mIMEState.mOpen),
    aContext.mHTMLInputType,
    aContext.mHTMLInputInputmode,
    aContext.mActionHint,
    static_cast<int32_t>(aAction.mCause),
    static_cast<int32_t>(aAction.mFocusChange));
}

NS_IMETHODIMP_(InputContext)
EmbedLitePuppetWidget::GetInputContext()
{
  mInputContext.mIMEState.mOpen = IMEState::OPEN_STATE_NOT_SUPPORTED;
  mInputContext.mNativeIMEContext = nullptr;
  if (mEmbed) {
    int32_t enabled, open;
    intptr_t nativeIMEContext;
    mEmbed->GetInputContext(&enabled, &open, &nativeIMEContext);
    mInputContext.mIMEState.mEnabled = static_cast<IMEState::Enabled>(enabled);
    mInputContext.mIMEState.mOpen = static_cast<IMEState::Open>(open);
    mInputContext.mNativeIMEContext = reinterpret_cast<void*>(nativeIMEContext);
  }
  return mInputContext;
}

NS_IMETHODIMP EmbedLitePuppetWidget::OnIMEFocusChange(bool aFocus)
{
  LOGT("aFocus:%i", aFocus);
  if (!aFocus) {
    mIMEComposing = false;
    mIMEComposingText.Truncate();
  }

  return NS_OK;
}

void
EmbedLitePuppetWidget::RemoveIMEComposition()
{
  // Remove composition on Gecko side
  if (!mIMEComposing) {
    return;
  }

  if (mEmbed) {
    mEmbed->ResetInputState();
  }

  nsRefPtr<EmbedLitePuppetWidget> kungFuDeathGrip(this);

  WidgetCompositionEvent textEvent(true, NS_COMPOSITION_CHANGE, this);
  textEvent.time = PR_Now() / 1000;
  textEvent.mData = mIMEComposingText;
  nsEventStatus status;
  DispatchEvent(&textEvent, status);

  WidgetCompositionEvent event(true, NS_COMPOSITION_END, this);
  event.time = PR_Now() / 1000;
  DispatchEvent(&event, status);
}

bool
EmbedLitePuppetWidget::ViewIsValid()
{
  return EmbedLiteApp::GetInstance()->GetViewByID(mId) != nullptr;
}

GLContext*
EmbedLitePuppetWidget::GetGLContext() const
{
  LOGT("this:%p, UseExternalContext:%d", this, sUseExternalGLContext);
  if (sUseExternalGLContext) {
    if (!sEGLLibrary.EnsureInitialized()) {
      return nullptr;
    }

    EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(mId);
    if (view && view->GetListener()->RequestCurrentGLContext()) {
      void* surface = sEGLLibrary.fGetCurrentSurface(LOCAL_EGL_DRAW);
      void* context = sEGLLibrary.fGetCurrentContext();
      nsRefPtr<GLContext> mozContext = GLContextProvider::CreateWrappingExisting(context, surface);
      if (!mozContext->Init()) {
        return nullptr;
      }
      return mozContext.forget().take();
    } else {
      NS_ERROR("Embedder wants to use external GL context without actually providing it!");
    }
  }
  return nullptr;
}

void
EmbedLitePuppetWidget::CreateGLContextEarly(uint32_t aViewId)
{
  LOGT("ViewId:%u", aViewId);
  MOZ_ASSERT(CompositorParent::IsInCompositorThread());
  MOZ_ASSERT(sRequestGLContextEarly);
  EmbedLiteView* view = EmbedLiteApp::GetInstance()->GetViewByID(aViewId);
  if (view) {
    view->GetListener()->RequestCurrentGLContext();
  } else {
    NS_WARNING("Trying to early create GL context for non existing view!");
  }
}

LayerManager*
EmbedLitePuppetWidget::GetLayerManager(PLayerTransactionChild* aShadowManager,
                                       LayersBackend aBackendHint,
                                       LayerManagerPersistence aPersistence,
                                       bool* aAllowRetaining)
{
  if (aAllowRetaining) {
    *aAllowRetaining = true;
  }

  if (Destroyed()) {
    NS_ERROR("It seems attempt to render widget after destroy");
    return nullptr;
  }


  if (mLayerManager) {
    return mLayerManager;
  }

  LOGT();

  nsIWidget* topWidget = GetTopLevelWidget();
  if (topWidget != this) {
    mLayerManager = topWidget->GetLayerManager();
  }

  if (mLayerManager) {
    return mLayerManager;
  }

  if (EmbedLiteApp::GetInstance()->GetType() == EmbedLiteApp::EMBED_INVALID) {
    printf("Create Layer Manager for Process View\n");
    mLayerManager = new ClientLayerManager(this);
    ShadowLayerForwarder* lf = mLayerManager->AsShadowForwarder();
    if (!lf->HasShadowManager() && aShadowManager) {
      lf->SetShadowManager(aShadowManager);
    }

    return mLayerManager;
  }

  if (!ViewIsValid()) {
    printf("Embed View has been destroyed early\n");
    mLayerManager = CreateBasicLayerManager();
    return mLayerManager;
  }

  EmbedLitePuppetWidget* topWindow = TopWindow();
  if (!topWindow) {
    printf_stderr(" -- no topwindow\n");
    mLayerManager = CreateBasicLayerManager();
    return mLayerManager;
  }

  mUseLayersAcceleration = ComputeShouldAccelerate(mUseLayersAcceleration);

  bool useCompositor = ShouldUseOffMainThreadCompositing();

  if (useCompositor) {
    CreateCompositor();
    if (mLayerManager) {
      return mLayerManager;
    }
    if (!ViewIsValid()) {
      printf(" -- Failed create compositor due to quick View destroy\n");
      mLayerManager = CreateBasicLayerManager();
      return mLayerManager;
    }
    // If we get here, then off main thread compositing failed to initialize.
    sFailedToCreateGLContext = true;
  }

  mLayerManager = new ClientLayerManager(this);
  mUseLayersAcceleration = false;

  return mLayerManager;
}

CompositorParent*
EmbedLitePuppetWidget::NewCompositorParent(int aSurfaceWidth, int aSurfaceHeight)
{
  LOGT();
  gfxPlatform::GetPlatform();
  return new EmbedLiteCompositorParent(this, true, aSurfaceWidth, aSurfaceHeight, mId);
}

void EmbedLitePuppetWidget::CreateCompositor()
{
  LOGT();
  gfxSize glSize = mEmbed->GetGLViewSize();
  CreateCompositor(glSize.width, glSize.height);
}

static void
CheckForBasicBackends(nsTArray<LayersBackend>& aHints)
{
  for (size_t i = 0; i < aHints.Length(); ++i) {
    if (aHints[i] == LayersBackend::LAYERS_BASIC &&
        !Preferences::GetBool("layers.offmainthreadcomposition.force-basic", false) &&
        !Preferences::GetBool("browser.tabs.remote", false)) {
      // basic compositor is not stable enough for regular use
      aHints[i] = LayersBackend::LAYERS_NONE;
    }
  }
}

void EmbedLitePuppetWidget::CreateCompositor(int aWidth, int aHeight)
{
  LOGT();
  mCompositorParent = NewCompositorParent(aWidth, aHeight);
  MessageChannel* parentChannel = mCompositorParent->GetIPCChannel();
  nsRefPtr<ClientLayerManager> lm = new ClientLayerManager(this);
  MessageLoop* childMessageLoop = CompositorParent::CompositorLoop();
  mCompositorChild = new CompositorChild(lm);
  mCompositorChild->Open(parentChannel, childMessageLoop, ipc::ChildSide);

  TextureFactoryIdentifier textureFactoryIdentifier;
  PLayerTransactionChild* shadowManager = nullptr;
  nsTArray<LayersBackend> backendHints;
  GetPreferredCompositorBackends(backendHints);

  CheckForBasicBackends(backendHints);

  bool success = false;
  if (!backendHints.IsEmpty()) {
    shadowManager = mCompositorChild->SendPLayerTransactionConstructor(
      backendHints, 0, &textureFactoryIdentifier, &success);
  }

  if (success) {
    ShadowLayerForwarder* lf = lm->AsShadowForwarder();
    if (!lf) {
      lm = nullptr;
      mCompositorChild = nullptr;
      return;
    }
    lf->SetShadowManager(shadowManager);
    lf->IdentifyTextureHost(textureFactoryIdentifier);
    ImageBridgeChild::IdentifyCompositorTextureHost(textureFactoryIdentifier);
    WindowUsesOMTC();

    mLayerManager = lm.forget();
  } else {
    // We don't currently want to support not having a LayersChild
    if (ViewIsValid()) {
      NS_RUNTIMEABORT("failed to construct LayersChild, and View still here");
    }
    lm = nullptr;
    mCompositorChild = nullptr;
  }
}

nsIntRect
EmbedLitePuppetWidget::GetNaturalBounds()
{
  return nsIntRect();
}

void
EmbedLitePuppetWidget::DrawWindowUnderlay(LayerManagerComposite *aManager, nsIntRect aRect)
{
  EmbedLiteCompositorParent* parent =
    static_cast<EmbedLiteCompositorParent*>(mCompositorParent.get());
  parent->DrawWindowUnderlay(aManager, aRect);
}

void
EmbedLitePuppetWidget::DrawWindowOverlay(LayerManagerComposite *aManager, nsIntRect aRect)
{
  EmbedLiteCompositorParent* parent =
    static_cast<EmbedLiteCompositorParent*>(mCompositorParent.get());
  parent->DrawWindowOverlay(aManager, aRect);
}

}  // namespace widget
}  // namespace mozilla

/*
 * Local declaration of android.view.Surface for the cpp backend.
 * Points at a wrapper header that includes the full IGraphicBufferProducer
 * definition — gui/view/Surface.h alone forward-declares it, which breaks
 * the generated code (sp<> needs the complete type).
 */
package android.view;

parcelable Surface cpp_header "vcam/view_surface_full.h";

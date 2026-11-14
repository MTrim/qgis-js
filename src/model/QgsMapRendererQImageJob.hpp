#pragma once

#include <qgsmaprendererjob.h>

#include <emscripten/bind.h>

#include "./QgsMapRendererJob.hpp"

// mobileGIS (4.2.2-mg11): copy into a JS Uint8ClampedArray while the local
// QImage is still alive. A typed_memory_view on it pointed into freed memory
// once the function returned (the allocator may overwrite the first bytes).
static emscripten::val QgsMapRendererQImageJob_renderedImage(QgsMapRendererQImageJob &job) {
  auto image = std::move(job.renderedImage()).convertToFormat(QImage::Format_RGBA8888);
  const auto view = emscripten::val(emscripten::typed_memory_view(
    image.width() * image.height() * 4, (const unsigned char *)image.constBits()));
  return emscripten::val::global("Uint8ClampedArray").new_(view);
}

EMSCRIPTEN_BINDINGS(QgsMapRendererQImageJob) {
  emscripten::class_<QgsMapRendererQImageJob, emscripten::base<QgsMapRendererJob>>(
    "QgsMapRendererQImageJob")
    .function("renderedImage", &QgsMapRendererQImageJob_renderedImage);
}

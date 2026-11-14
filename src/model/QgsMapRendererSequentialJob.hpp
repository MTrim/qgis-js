#pragma once

#include <qgsmaprenderersequentialjob.h>

#include <emscripten/bind.h>

#include "./QgsMapRendererQImageJob.hpp"

EMSCRIPTEN_BINDINGS(QgsMapRendererSequentialJob) {
  emscripten::class_<QgsMapRendererSequentialJob, emscripten::base<QgsMapRendererQImageJob>>(
    "QgsMapRendererSequentialJob");
}

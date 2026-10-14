// Print layouts: list the layouts stored in the loaded project and export
// them (optionally per atlas feature) as PDF or PNG.
//
// Layouts are read into the project's layout manager by QgsProject::read(),
// so no .qpt template is needed. Export runs synchronously in the calling
// thread; map items render with QgsMapRendererCustomPainterJob, which does
// not need worker threads.

#include <optional>
#include <string>
#include <vector>

#include <qgsexpressioncontextutils.h>
#include <qgslayoutatlas.h>
#include <qgslayoutexporter.h>
#include <qgslayoutmanager.h>
#include <qgslayoutpagecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include <QDir>
#include <QFile>
#include <QStringList>

#include <emscripten/bind.h>
#include <emscripten/val.h>

namespace {

QgsPrintLayout *findLayout(const std::string &name) {
  return dynamic_cast<QgsPrintLayout *>(
    QgsProject::instance()->layoutManager()->layoutByName(QString::fromStdString(name)));
}

emscripten::val bytesToJs(const QByteArray &bytes) {
  // copy into a JS-owned Uint8Array, the QByteArray dies with this call
  emscripten::val view(
    emscripten::typed_memory_view(bytes.size(), (const unsigned char *)bytes.constData()));
  return emscripten::val::global("Uint8Array").new_(view);
}

emscripten::val failure(const QString &message) {
  emscripten::val result = emscripten::val::object();
  result.set("success", false);
  result.set("error", message.toStdString());
  return result;
}

// Restores the atlas settings that exportLayout() changes temporarily.
struct AtlasState {
  QgsLayoutAtlas *atlas;
  bool enabled;
  bool filterFeatures;
  QString filterExpression;
  ~AtlasState() {
    atlas->setFilterFeatures(filterFeatures);
    QString error;
    atlas->setFilterExpression(filterExpression, error);
    atlas->setEnabled(enabled);
  }
};

} // namespace

// [{ name, pages, atlas: { enabled, coverageLayerId, coverageLayerName, count } }]
emscripten::val QgisLayouts_layouts() {
  emscripten::val list = emscripten::val::array();
  const QList<QgsPrintLayout *> layouts = QgsProject::instance()->layoutManager()->printLayouts();
  for (QgsPrintLayout *layout : layouts) {
    emscripten::val entry = emscripten::val::object();
    entry.set("name", layout->name().toStdString());
    entry.set("pages", layout->pageCollection()->pageCount());

    QgsLayoutAtlas *atlas = layout->atlas();
    emscripten::val atlasInfo = emscripten::val::object();
    const bool atlasEnabled = atlas && atlas->enabled() && atlas->coverageLayer();
    atlasInfo.set("enabled", atlasEnabled);
    if (atlasEnabled) {
      atlasInfo.set("coverageLayerId", atlas->coverageLayer()->id().toStdString());
      atlasInfo.set("coverageLayerName", atlas->coverageLayer()->name().toStdString());
      atlasInfo.set("filterFeatures", atlas->filterFeatures());
    }
    entry.set("atlas", atlasInfo);
    list.call<void>("push", entry);
  }
  return list;
}

// options: { format?: "pdf" | "png", dpi?: number, page?: number (png only),
//            atlasFeatureIds?: number[] (atlas layouts only, empty = all) }
// returns { success, data?: Uint8Array, error?: string, count?: number }
emscripten::val QgisLayouts_exportLayout(std::string name, std::optional<emscripten::val> options) {
  QgsPrintLayout *layout = findLayout(name);
  if (!layout)
    return failure(QStringLiteral("Layout not found: %1").arg(QString::fromStdString(name)));

  std::string format = "pdf";
  double dpi = 150;
  int page = 0;
  std::optional<std::vector<long long>> featureIds;
  if (options.has_value() && !options->isUndefined() && !options->isNull()) {
    if (!(*options)["format"].isUndefined())
      format = (*options)["format"].as<std::string>();
    if (!(*options)["dpi"].isUndefined())
      dpi = (*options)["dpi"].as<double>();
    if (!(*options)["page"].isUndefined())
      page = (*options)["page"].as<int>();
    emscripten::val ids = (*options)["atlasFeatureIds"];
    if (!ids.isUndefined() && !ids.isNull()) {
      std::vector<long long> list;
      const int length = ids["length"].as<int>();
      for (int i = 0; i < length; i++)
        list.push_back(ids[i].as<double>());
      featureIds = list;
    }
  }
  if (format != "pdf" && format != "png")
    return failure(QStringLiteral("Unknown format: %1").arg(QString::fromStdString(format)));

  const QString path = QStringLiteral("/tmp/qgis-js-layout.%1").arg(QString::fromStdString(format));
  for (const QString &old : QDir(QStringLiteral("/tmp")).entryList(QStringList() << QStringLiteral("qgis-js-layout*"), QDir::Files))
    QFile::remove(QStringLiteral("/tmp/") + old);

  QgsLayoutAtlas *atlas = layout->atlas();
  const bool useAtlas = atlas && atlas->enabled() && atlas->coverageLayer();

  QgsLayoutExporter::ExportResult result = QgsLayoutExporter::Success;
  QString error;
  int count = 1;

  if (format == "pdf") {
    QgsLayoutExporter::PdfExportSettings settings;
    settings.dpi = dpi;
    settings.rasterizeWholeImage = false;
    // no GeoPDF / XMP: both reopen the file with GDAL's PDF driver
    settings.appendGeoreference = false;
    settings.exportMetadata = false;
    settings.textRenderFormat = Qgis::TextRenderFormat::AlwaysText;

    if (useAtlas) {
      AtlasState restore{atlas, atlas->enabled(), atlas->filterFeatures(), atlas->filterExpression()};
      if (featureIds.has_value() && !featureIds->empty()) {
        QStringList ids;
        for (long long id : *featureIds)
          ids << QString::number(id);
        atlas->setFilterFeatures(true);
        if (!atlas->setFilterExpression(QStringLiteral("$id IN (%1)").arg(ids.join(',')), error))
          return failure(error);
      }
      atlas->updateFeatures();
      count = atlas->count();
      if (count == 0)
        return failure(QStringLiteral("The atlas has no features to export"));
      result = QgsLayoutExporter::exportToPdf(atlas, path, settings, error);
    } else {
      layout->refresh();
      QgsLayoutExporter exporter(layout);
      result = exporter.exportToPdf(path, settings);
    }
  } else {
    // PNG: one page of the layout (or of the first atlas feature)
    QgsLayoutExporter::ImageExportSettings settings;
    settings.dpi = dpi;
    settings.pages = QList<int>() << page;
    if (useAtlas) {
      AtlasState restore{atlas, atlas->enabled(), atlas->filterFeatures(), atlas->filterExpression()};
      if (featureIds.has_value() && !featureIds->empty()) {
        atlas->setFilterFeatures(true);
        if (!atlas->setFilterExpression(QStringLiteral("$id = %1").arg((*featureIds)[0]), error))
          return failure(error);
      }
      atlas->updateFeatures();
      if (atlas->count() == 0)
        return failure(QStringLiteral("The atlas has no features to export"));
      atlas->beginRender();
      atlas->first();
      QgsLayoutExporter exporter(layout);
      result = exporter.exportToImage(path, settings);
      atlas->endRender();
    } else {
      layout->refresh();
      QgsLayoutExporter exporter(layout);
      result = exporter.exportToImage(path, settings);
    }
  }

  if (result != QgsLayoutExporter::Success)
    return failure(error.isEmpty() ? QStringLiteral("Export failed (code %1)").arg(static_cast<int>(result)) : error);

  // exportToImage suffixes the file name for pages after the first
  // ("name_2.png"), so pick whatever the exporter actually wrote
  QString written = path;
  if (!QFile::exists(written)) {
    const QStringList files = QDir(QStringLiteral("/tmp")).entryList(
      QStringList() << QStringLiteral("qgis-js-layout*.%1").arg(QString::fromStdString(format)), QDir::Files);
    if (!files.isEmpty())
      written = QStringLiteral("/tmp/") + files.first();
  }
  QFile file(written);
  if (!file.open(QIODevice::ReadOnly) || file.size() == 0)
    return failure(QStringLiteral("Export produced no file"));
  const QByteArray bytes = file.readAll();
  file.close();
  QFile::remove(written);

  emscripten::val out = emscripten::val::object();
  out.set("success", true);
  out.set("data", bytesToJs(bytes));
  out.set("count", count);
  return out;
}

EMSCRIPTEN_BINDINGS(QgisLayouts) {
  emscripten::function("layouts", &QgisLayouts_layouts);
  emscripten::function("exportLayout", &QgisLayouts_exportLayout);
}

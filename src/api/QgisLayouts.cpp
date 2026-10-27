// Print layouts: list the layouts stored in the loaded project and export
// them (optionally per atlas feature) as PDF or PNG.
//
// Layouts are read into the project's layout manager by QgsProject::read(),
// so no .qpt template is needed. Export runs synchronously in the calling
// thread; map items render with QgsMapRendererCustomPainterJob, which does
// not need worker threads.

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <qgsexpressioncontextutils.h>
#include <qgsfillsymbol.h>
#include <qgslayoutatlas.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitemregistry.h>
#include <qgslayoutmeasurementconverter.h>
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

// Page sizes and map items of the layout in its current state (current
// atlas feature), one entry per page, in mm. Lets the caller draw what QGIS
// cannot render here (online layers) underneath / on top of each map item.
void appendPageInfo(QgsLayout *layout, emscripten::val &pages) {
  const QgsLayoutMeasurementConverter &conv = layout->renderContext().measurementConverter();
  QList<QgsLayoutItemMap *> maps;
  layout->layoutItems(maps);
  QgsLayoutPageCollection *collection = layout->pageCollection();
  for (int i = 0; i < collection->pageCount(); i++) {
    const QgsLayoutSize size = conv.convert(collection->page(i)->pageSize(), Qgis::LayoutUnit::Millimeters);
    emscripten::val page = emscripten::val::object();
    page.set("width", size.width());
    page.set("height", size.height());
    emscripten::val list = emscripten::val::array();
    for (int index = 0; index < maps.size(); index++) {
      QgsLayoutItemMap *map = maps[index];
      if (map->page() != i || !map->isVisible())
        continue;
      const QgsLayoutPoint pos = conv.convert(map->pagePositionWithUnits(), Qgis::LayoutUnit::Millimeters);
      const QgsLayoutSize msize = conv.convert(map->sizeWithUnits(), Qgis::LayoutUnit::Millimeters);
      const QgsRectangle extent = map->extent();
      emscripten::val item = emscripten::val::object();
      item.set("id", map->id().toStdString());
      item.set("index", index);
      item.set("x", pos.x());
      item.set("y", pos.y());
      item.set("width", msize.width());
      item.set("height", msize.height());
      emscripten::val ext = emscripten::val::array();
      ext.call<void>("push", extent.xMinimum());
      ext.call<void>("push", extent.yMinimum());
      ext.call<void>("push", extent.xMaximum());
      ext.call<void>("push", extent.yMaximum());
      item.set("extent", ext);
      item.set("rotation", map->mapRotation());
      item.set("itemRotation", map->itemRotation());
      item.set("crs", map->crs().authid().toStdString());
      item.set("scale", map->scale());
      list.call<void>("push", item);
    }
    page.set("maps", list);
    pages.call<void>("push", page);
  }
}

// Switches map item and page backgrounds off for the export and back on
// afterwards, so the caller can place content underneath the map items.
struct TransparentMaps {
  QgsLayout *layout = nullptr;
  std::vector<std::pair<QgsLayoutItemMap *, bool>> saved;
  std::unique_ptr<QgsFillSymbol> pageSymbol;
  TransparentMaps(QgsLayout *layout, bool active) {
    if (!active)
      return;
    this->layout = layout;
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems(maps);
    for (QgsLayoutItemMap *map : maps) {
      saved.emplace_back(map, map->hasBackground());
      map->setBackgroundEnabled(false);
    }
    QgsLayoutPageCollection *pages = layout->pageCollection();
    if (pages->pageStyleSymbol())
      pageSymbol.reset(pages->pageStyleSymbol()->clone());
    std::unique_ptr<QgsFillSymbol> clear(QgsFillSymbol::createSimple(
      {{QStringLiteral("color"), QStringLiteral("0,0,0,0")}, {QStringLiteral("outline_style"), QStringLiteral("no")}}));
    pages->setPageStyleSymbol(clear.get());
  }
  ~TransparentMaps() {
    for (auto &[map, background] : saved)
      map->setBackgroundEnabled(background);
    if (layout && pageSymbol)
      layout->pageCollection()->setPageStyleSymbol(pageSymbol.get());
  }
};

// Splits the page at the lowest map item (z order), so content can be placed
// between the two halves: "below" exports only items under the maps, "above"
// the maps and everything on top of them. Needed with transparentMaps: a
// filled frame under the map would otherwise cover whatever is put beneath
// the whole page (mobileVIEW, MEGIS layout "Ausgabe - A3", 27.10.2026).
// Pages and groups stay untouched, hidden items are restored afterwards.
struct LayerSplit {
  std::vector<QgsLayoutItem *> hidden;
  LayerSplit(QgsLayout *layout, const std::string &mode) {
    if (mode != "below" && mode != "above")
      return;
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems(maps);
    if (maps.isEmpty())
      return;
    double minZ = std::numeric_limits<double>::max();
    for (QgsLayoutItemMap *map : maps)
      minZ = std::min(minZ, map->zValue());
    QList<QgsLayoutItem *> items;
    layout->layoutItems(items);
    for (QgsLayoutItem *item : items) {
      if (item->type() == QgsLayoutItemRegistry::LayoutPage || item->type() == QgsLayoutItemRegistry::LayoutGroup)
        continue;
      const bool below = item->zValue() < minZ;
      if ((mode == "below") != below && item->isVisible()) {
        item->setVisibility(false);
        hidden.push_back(item);
      }
    }
  }
  ~LayerSplit() {
    for (QgsLayoutItem *item : hidden)
      item->setVisibility(true);
  }
};

// Moves map items to a new center and scale for the export and restores the
// stored extents afterwards. Not for atlas layouts, the atlas sets the extent.
struct MapOverrides {
  std::vector<std::pair<QgsLayoutItemMap *, QgsRectangle>> saved;
  MapOverrides(QgsLayout *layout, const std::optional<emscripten::val> &list) {
    if (!list.has_value())
      return;
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems(maps);
    const int length = (*list)["length"].as<int>();
    for (int i = 0; i < length; i++) {
      emscripten::val entry = (*list)[i];
      const int index = entry["index"].as<int>();
      if (index < 0 || index >= maps.size())
        continue;
      QgsLayoutItemMap *map = maps[index];
      emscripten::val center = entry["center"];
      const double x = center[0].as<double>();
      const double y = center[1].as<double>();
      const double scale = entry["scale"].as<double>();
      if (!(scale > 0))
        continue;
      saved.emplace_back(map, map->extent());
      // zoomToExtent keeps the item's aspect ratio, setScale then scales
      // around the center
      map->zoomToExtent(QgsRectangle(x - 1, y - 1, x + 1, y + 1));
      map->setScale(scale);
    }
  }
  ~MapOverrides() {
    for (auto &[map, extent] : saved)
      map->setExtent(extent);
  }
};

} // namespace

// [{ name, pages, atlas: { enabled, coverageLayerId, coverageLayerName },
//    pageInfo: [{ width, height, maps: [...] }] (see appendPageInfo) }]
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
    // map items as stored in the project (unrotated extents, mm on the page)
    emscripten::val pageInfo = emscripten::val::array();
    appendPageInfo(layout, pageInfo);
    entry.set("pageInfo", pageInfo);
    list.call<void>("push", entry);
  }
  return list;
}

// options: { format?: "pdf" | "png", dpi?: number, page?: number (png only),
//            atlasFeatureIds?: number[] (atlas layouts only, empty = all),
//            transparentMaps?: boolean (no map item and page background, PDF only),
//            layer?: "below" | "above" (PDF only, see LayerSplit; echoed back),
//            maps?: [{ index, center: [x, y], scale }] (move map items, not atlas) }
// returns { success, data?: Uint8Array, error?: string, count?: number,
//           pages?: [{ width, height, maps: [{ id, x, y, width, height, extent,
//                     rotation, itemRotation, crs, scale }] }] (PDF only, mm) }
emscripten::val QgisLayouts_exportLayout(std::string name, std::optional<emscripten::val> options) {
  QgsPrintLayout *layout = findLayout(name);
  if (!layout)
    return failure(QStringLiteral("Layout not found: %1").arg(QString::fromStdString(name)));

  std::string format = "pdf";
  double dpi = 150;
  int page = 0;
  bool transparent = false;
  std::string layer;
  std::optional<std::vector<long long>> featureIds;
  std::optional<emscripten::val> mapList;
  if (options.has_value() && !options->isUndefined() && !options->isNull()) {
    if (!(*options)["format"].isUndefined())
      format = (*options)["format"].as<std::string>();
    if (!(*options)["dpi"].isUndefined())
      dpi = (*options)["dpi"].as<double>();
    if (!(*options)["page"].isUndefined())
      page = (*options)["page"].as<int>();
    if (!(*options)["transparentMaps"].isUndefined())
      transparent = (*options)["transparentMaps"].as<bool>();
    if (!(*options)["layer"].isUndefined())
      layer = (*options)["layer"].as<std::string>();
    emscripten::val overrides = (*options)["maps"];
    if (!overrides.isUndefined() && !overrides.isNull())
      mapList = overrides;
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
  emscripten::val pages = emscripten::val::array();

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
      // Map extents per atlas feature, in the order the exporter writes them
      atlas->beginRender();
      for (int i = 0; i < count; i++) {
        if (atlas->seekTo(i))
          appendPageInfo(layout, pages);
      }
      atlas->endRender();
      TransparentMaps noBackground(layout, transparent);
      LayerSplit split(layout, layer);
      result = QgsLayoutExporter::exportToPdf(atlas, path, settings, error);
    } else {
      MapOverrides moved(layout, mapList);
      layout->refresh();
      appendPageInfo(layout, pages);
      TransparentMaps noBackground(layout, transparent);
      LayerSplit split(layout, layer);
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
      MapOverrides moved(layout, mapList);
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
  if (format == "pdf")
    out.set("pages", pages);
  if (format == "pdf" && (layer == "below" || layer == "above"))
    out.set("layer", layer);
  return out;
}

EMSCRIPTEN_BINDINGS(QgisLayouts) {
  emscripten::function("layouts", &QgisLayouts_layouts);
  emscripten::function("exportLayout", &QgisLayouts_exportLayout);
}

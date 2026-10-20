import {
  QgisModelConstructors,
  QgsMapRendererParallelJob,
  QgsRectangle,
  QgsLayerTreeGroup,
  QgsVectorLayer,
} from "./QgisModel";

import type { IdentifyMode, IdentifyResult } from "./QgisIdentify";

export interface LayerDefinitionResult {
  success: boolean;
  errorMessage: string;
}

/**
 * Common QGIS API which exposes the {@link QgisModelConstructors} and methods that can be accessed from both the public {@link QgisApi} and the {@link InternalQgisApi}.
 */
export interface CommonQgisApi extends QgisModelConstructors {
  /**
   * Loads a QGIS project file.
   *
   * @param filename - The path to the QGIS project file.
   * @returns true if the project was loaded successfully, false otherwise.
   */
  loadProject(filename: string): boolean;

  /**
   * Drops all layers, layouts and variables from the current project,
   * leaving an empty project ready to be populated from JS — useful when
   * building a project dynamically with `QgsVectorLayer.createMemoryLayer`
   * and `addMapLayer`.
   */
  clearProject(): void;

  /**
   * Register a layer (typically created by
   * `QgsVectorLayer.createMemoryLayer`) with the current project. The
   * project takes ownership of the underlying QGIS layer — do NOT call
   * `addMapLayer` twice with the same wrapper.
   *
   * @returns true if the layer was added.
   */
  addMapLayer(layer: QgsVectorLayer): boolean;

  /**
   * Loads a QGIS Layer Definition (.qlr) file into the project.
   *
   * @param path - Path to the .qlr file on the virtual filesystem.
   * @param targetGroup - Optional target group to load into. Defaults to the root group.
   * @returns Result indicating success or failure with an error message.
   */
  loadLayerDefinition(
    path: string,
    targetGroup?: QgsLayerTreeGroup,
  ): LayerDefinitionResult;

  /**
   * Returns the full extent of the loaded project.
   *
   * @returns The full extent of the loaded project.
   */
  fullExtent(layerIds?: string[]): QgsRectangle;

  /**
   * Returns the SRID of the loaded project.
   *
   * @returns The SRID of the loaded project.
   */
  srid(): string;

  /**
   * Transforms a rectangle from one SRID to another.
   *
   * @param rect - The rectangle to transform.
   * @param inputSrid - The SRID of the input rectangle.
   * @param outputSrid - The SRID of the output rectangle.
   * @returns The transformed rectangle.
   */
  transformRectangle(
    rect: QgsRectangle,
    inputSrid: string,
    outputSrid: string,
  ): QgsRectangle;

  /**
   * Gets the current map theme of the current project.
   *
   * @returns The name of the current map theme. An empty string if no map theme is set.
   */
  getMapTheme(): string;

  /**
   * Sets a map theme of the current project.
   *
   * @param
   * @returns true if the map theme was loaded successfully, false otherwise.
   */
  setMapTheme(mapTheme: string): boolean;

  /**
   * Returns the global expression variables.
   *
   * These are system-level variables such as `qgis_version`, `qgis_locale`,
   * `qgis_os_name`, `qgis_platform`, etc.
   *
   * @returns A record of variable names to their string values.
   */
  globalVariables(): Record<string, string>;

  /**
   * Returns the project expression variables.
   *
   * These are project-level variables such as `project_crs`, `project_title`,
   * `project_filename`, `layer_ids`, `project_area_units`, etc.
   * Only available after a project has been loaded.
   *
   * @returns A record of variable names to their string values.
   */
  projectVariables(): Record<string, string>;

  /**
   * Sets global expression variables.
   *
   * Custom variables that will be available in all expression contexts.
   * Note: This replaces all custom global variables.
   *
   * @param variables - A record of variable names to their string values.
   */
  setGlobalVariables(variables: Record<string, string>): void;

  /**
   * Sets project expression variables.
   *
   * Custom variables that will be available in the project's expression contexts.
   * Note: This replaces all custom project variables.
   *
   * @param variables - A record of variable names to their string values.
   */
  setProjectVariables(variables: Record<string, string>): void;

  /**
   * Returns the root of the project's layer tree.
   *
   * @returns The root layer tree node.
   */
  layerTreeRoot(): QgsLayerTreeGroup;

  /**
   * Renders the full project legend as a PNG image.
   *
   * @param dpi - The DPI for rendering the legend.
   * @returns A base64 data URL of the legend PNG, or an empty string if the legend is empty.
   */
  renderLegend(dpi: number, layerIds?: string[]): string;

  /**
   * Lists the print layouts stored in the loaded project (layout manager).
   *
   * @returns One entry per print layout with its page count and atlas settings.
   */
  layouts(): PrintLayoutInfo[];

  /**
   * Exports a print layout of the loaded project.
   *
   * Runs synchronously. For layouts with an enabled atlas, all atlas features
   * (or only `atlasFeatureIds`) are exported into one PDF.
   *
   * @param name - The name of the layout as listed by {@link layouts}.
   * @param options - Output format, resolution, page (PNG) and atlas features.
   * @returns The exported file as bytes, or an error message.
   */
  exportLayout(name: string, options?: LayoutExportOptions): LayoutExportResult;

  /**
   * Renders an image of the loaded project and provides a QgsMapRendererParallelJob object to monitor the rendering progress and to retrieve preview images.
   *
   * @param srid - The SRID of the image.
   * @param extent - The extent of the image.
   * @param width - The width of the image.
   * @param height - The height of the image.
   * @param pixelRatio - The optional pixel ratio of the image which defaults to 1.
   * @param layerIds - Optional list of layer ids to render (defaults to all visible layers).
   * @param rotation - Optional map rotation in degrees, clockwise (like QGIS Desktop). The extent is
   *   then the unrotated extent around the map center; labels and point symbols stay upright.
   * @returns A QgsMapRendererParallelJob object that can be used to monitor the rendering progress and to retrieve preview images.
   */
  renderJob(
    srid: string,
    extent: QgsRectangle,
    width: number,
    height: number,
    pixelRatio: number,
    layerIds?: string[],
    rotation?: number,
  ): QgsMapRendererParallelJob;

  /**
   * Identifies vector features intersecting a rectangle, walking visible
   * layers in top-down render order. Mirrors the core of QGIS's
   * `QgsMapToolIdentify::identifyVectorLayer` without the GUI dependency.
   *
   * The rect is interpreted in `rectSrid`; per-layer queries reproject it
   * into each layer's CRS. Returned feature geometries are in `rectSrid`.
   *
   * Uses `setFilterRect` plus exact geometry intersection
   * (`Qgis::FeatureRequestFlag::ExactIntersect`). When the data provider
   * supports a spatial index, the rectangle hint lets it skip a full-layer
   * scan; providers without an index will fall back to scanning.
   *
   * @param rect - The hit-box rectangle.
   * @param rectSrid - SRID of the rectangle (e.g. the map's project CRS).
   * @param mode - See {@link IdentifyMode}.
   */
  identify(
    rect: QgsRectangle,
    rectSrid: string,
    mode: IdentifyMode,
  ): IdentifyResult[];
}

/**
 * The QgisApiAdapter provides convenience methods in addition to the CommonQgisApi.
 */
export interface QgisApiAdapter {
  /**
   * Renders an image of the loaded project.
   *
   * @param srid - The SRID of the image.
   * @param extent - The extent of the image.
   * @param width - The width of the image.
   * @param height - The height of the image.
   * @param pixelRatio - The optional pixel ratio of the image which defaults to 1.
   * @returns The rendered tile as an ImageData object.
   */
  renderImage(
    srid: string,
    extent: QgsRectangle,
    width: number,
    height: number,
    pixelRatio?: number,
    layerIds?: string[],
  ): Promise<ImageData>;

  /**
   * Renders a tile of the loaded project in the XYZ scheme (EPSG:3857).
   *
   * @param x - The x coordinate of the tile.
   * @param y - The y coordinate of the tile.
   * @param z - The z coordinate of the tile.
   * @param tileSize - The optional size of the tile which defaults to 256.
   * @param pixelRatio - The optional pixel ratio of the tile which defaults to 1.
   * @param extentBufferFactor - The optional extent buffer factor of the tile which defaults to 0.
   * @param layerIds - Optional array of layer IDs to render. If omitted, renders all visible layers.
   * @returns The rendered tile as an ImageData object.
   */
  renderXYZTile(
    x: number,
    y: number,
    z: number,
    tileSize?: number,
    pixelRatio?: number,
    extentBufferFactor?: number,
    layerIds?: string[],
  ): Promise<ImageData>;

  /**
   * Returns the map themes of the loaded project.
   *
   * @returns The map themes of the loaded project.
   */
  mapThemes(): readonly string[];
}

/**
 * Interface representing the public QgisApi.
 */
export interface QgisApi extends CommonQgisApi, QgisApiAdapter {}

/**
 * The internal Qgis API which can be accessed from the QgisRuntimeModule
 *
 * This interface is not a stable API and may change at any time. Use the public {@link QgisApi} instead.
 */
export interface InternalQgisApi extends CommonQgisApi {
  renderImage(
    srid: string,
    extent: QgsRectangle,
    width: number,
    height: number,
    pixelRatio: number,
    callback: (tileData: ArrayBufferLike) => void,
    layerIds?: string[],
  ): void;
  renderXYZTile(
    x: number,
    y: number,
    z: number,
    tileSize: number,
    pixelRatio: number,
    extentBuffer: number,
    callback: (tileData: ArrayBufferLike) => void,
    layerIds?: string[],
  ): number;
  mapThemes(): any;
}

/**
 * A print layout of the loaded project.
 */
export interface PrintLayoutInfo {
  name: string;
  pages: number;
  atlas: {
    enabled: boolean;
    coverageLayerId?: string;
    coverageLayerName?: string;
    filterFeatures?: boolean;
  };
  /** Pages with their map items as stored in the project. */
  pageInfo: LayoutPageInfo[];
}

/**
 * Options for {@link CommonQgisApi.exportLayout}.
 */
export interface LayoutExportOptions {
  /** Output format, defaults to "pdf". */
  format?: "pdf" | "png";
  /** Resolution in dots per inch, defaults to 150. */
  dpi?: number;
  /** Zero-based page for PNG output, defaults to 0. */
  page?: number;
  /** Atlas layouts only: feature ids of the coverage layer to export; empty or omitted exports all. */
  atlasFeatureIds?: number[];
  /** PDF only: draw map items and pages without their background, so content can be placed underneath. */
  transparentMaps?: boolean;
  /** Non-atlas layouts: move map items (by {@link LayoutMapInfo.index}) to a new center and scale for this export. */
  maps?: { index: number; center: [number, number]; scale: number }[];
}

/**
 * A map item on an exported page, in millimetres from the page's top left corner.
 */
export interface LayoutMapInfo {
  id: string;
  /** Position among the layout's map items, used by {@link LayoutExportOptions.maps}. */
  index: number;
  x: number;
  y: number;
  width: number;
  height: number;
  /** Unrotated map extent [xmin, ymin, xmax, ymax] in `crs`. */
  extent: [number, number, number, number];
  /** Map rotation in degrees, clockwise. */
  rotation: number;
  /** Rotation of the item on the page in degrees, clockwise. */
  itemRotation: number;
  crs: string;
  scale: number;
}

/**
 * An exported page with its size in millimetres and its map items.
 */
export interface LayoutPageInfo {
  width: number;
  height: number;
  maps: LayoutMapInfo[];
}

/**
 * Result of {@link CommonQgisApi.exportLayout}.
 */
export interface LayoutExportResult {
  success: boolean;
  data?: Uint8Array;
  error?: string;
  /** Number of atlas features (pages groups) exported, 1 without atlas. */
  count?: number;
  /** PDF only: one entry per page of the output, in output order. */
  pages?: LayoutPageInfo[];
}


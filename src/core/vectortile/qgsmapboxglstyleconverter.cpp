/***************************************************************************
  qgsmapboxglstyleconverter.cpp
  --------------------------------------
  Date                 : September 2020
  Copyright            : (C) 2020 by Nyall Dawson
  Email                : nyall dot dawson at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


/*
 * Ported from original work by Martin Dobias, and extended by the MapTiler team!
 */

#include "qgsmapboxglstyleconverter.h"
#include "qgsvectortilebasicrenderer.h"
#include "qgsvectortilebasiclabeling.h"
#include "qgssymbollayer.h"
#include "qgssymbollayerutils.h"
#include "qgslogger.h"
#include "qgsfillsymbollayer.h"
#include "qgslinesymbollayer.h"
#include "qgsfontutils.h"
#include "qgsjsonutils.h"
#include "qgspainteffect.h"
#include "qgseffectstack.h"
#include "qgsblureffect.h"
#include "qgsmarkersymbollayer.h"


QgsMapBoxGlStyleConverter::QgsMapBoxGlStyleConverter()
{
}

QgsMapBoxGlStyleConverter::Result QgsMapBoxGlStyleConverter::convert( const QVariantMap &style, QgsMapBoxGlStyleConversionContext *context )
{
  mError.clear();
  mWarnings.clear();
  if ( style.contains( QStringLiteral( "layers" ) ) )
  {
    parseLayers( style.value( QStringLiteral( "layers" ) ).toList(), context );
  }
  else
  {
    mError = QObject::tr( "Could not find layers list in JSON" );
    return NoLayerList;
  }
  return Success;
}

QgsMapBoxGlStyleConverter::Result QgsMapBoxGlStyleConverter::convert( const QString &style, QgsMapBoxGlStyleConversionContext *context )
{
  return convert( QgsJsonUtils::parseJson( style ).toMap(), context );
}

QgsMapBoxGlStyleConverter::~QgsMapBoxGlStyleConverter() = default;

void QgsMapBoxGlStyleConverter::parseLayers( const QVariantList &layers, QgsMapBoxGlStyleConversionContext *context )
{
  std::unique_ptr< QgsMapBoxGlStyleConversionContext > tmpContext;
  if ( !context )
  {
    tmpContext = qgis::make_unique< QgsMapBoxGlStyleConversionContext >();
    context = tmpContext.get();
  }

  QList<QgsVectorTileBasicRendererStyle> rendererStyles;
  QList<QgsVectorTileBasicLabelingStyle> labelingStyles;

  for ( const QVariant &layer : layers )
  {
    const QVariantMap jsonLayer = layer.toMap();

    const QString layerType = jsonLayer.value( QStringLiteral( "type" ) ).toString();
    if ( layerType == QLatin1String( "background" ) )
      continue;

    const QString styleId = jsonLayer.value( QStringLiteral( "id" ) ).toString();
    const QString layerName = jsonLayer.value( QStringLiteral( "source-layer" ) ).toString();

    const int minZoom = jsonLayer.value( QStringLiteral( "minzoom" ), QStringLiteral( "-1" ) ).toInt();
    const int maxZoom = jsonLayer.value( QStringLiteral( "maxzoom" ), QStringLiteral( "-1" ) ).toInt();

    const bool enabled = jsonLayer.value( QStringLiteral( "visibility" ) ).toString() != QLatin1String( "none" );

    QString filterExpression;
    if ( jsonLayer.contains( QStringLiteral( "filter" ) ) )
    {
      filterExpression = parseExpression( jsonLayer.value( QStringLiteral( "filter" ) ).toList(), *context );
    }

    QgsVectorTileBasicRendererStyle rendererStyle;
    QgsVectorTileBasicLabelingStyle labelingStyle;

    bool hasRendererStyle = false;
    bool hasLabelingStyle = false;
    if ( layerType == QLatin1String( "fill" ) )
    {
      hasRendererStyle = parseFillLayer( jsonLayer, rendererStyle, *context );
    }
    else if ( layerType == QLatin1String( "line" ) )
    {
      hasRendererStyle = parseLineLayer( jsonLayer, rendererStyle, *context );
    }
    else if ( layerType == QLatin1String( "symbol" ) )
    {
      parseSymbolLayer( jsonLayer, rendererStyle, hasRendererStyle, labelingStyle, hasLabelingStyle, *context );
    }
    else
    {
      mWarnings << QObject::tr( "Skipping unknown layer type: %1" ).arg( layerType );
      QgsDebugMsg( mWarnings.constLast() );
      continue;
    }

    if ( hasRendererStyle )
    {
      rendererStyle.setStyleName( styleId );
      rendererStyle.setLayerName( layerName );
      rendererStyle.setFilterExpression( filterExpression );
      rendererStyle.setMinZoomLevel( minZoom );
      rendererStyle.setMaxZoomLevel( maxZoom );
      rendererStyle.setEnabled( enabled );
      rendererStyles.append( rendererStyle );
    }

    if ( hasLabelingStyle )
    {
      labelingStyle.setStyleName( styleId );
      labelingStyle.setLayerName( layerName );
      labelingStyle.setFilterExpression( filterExpression );
      labelingStyle.setMinZoomLevel( minZoom );
      labelingStyle.setMaxZoomLevel( maxZoom );
      labelingStyle.setEnabled( enabled );
      labelingStyles.append( labelingStyle );
    }

    mWarnings.append( context->warnings() );
    context->clearWarnings();
  }

  mRenderer = qgis::make_unique< QgsVectorTileBasicRenderer >();
  QgsVectorTileBasicRenderer *renderer = dynamic_cast< QgsVectorTileBasicRenderer *>( mRenderer.get() );
  renderer->setStyles( rendererStyles );

  mLabeling = qgis::make_unique< QgsVectorTileBasicLabeling >();
  QgsVectorTileBasicLabeling *labeling = dynamic_cast< QgsVectorTileBasicLabeling * >( mLabeling.get() );
  labeling->setStyles( labelingStyles );
}

bool QgsMapBoxGlStyleConverter::parseFillLayer( const QVariantMap &jsonLayer, QgsVectorTileBasicRendererStyle &style, QgsMapBoxGlStyleConversionContext &context )
{
  if ( !jsonLayer.contains( QStringLiteral( "paint" ) ) )
  {
    context.pushWarning( QObject::tr( "Style layer %1 has no paint property, skipping" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
    return false;
  }

  const QVariantMap jsonPaint = jsonLayer.value( QStringLiteral( "paint" ) ).toMap();

  QgsPropertyCollection ddProperties;
  QgsPropertyCollection ddRasterProperties;

  // fill color
  QColor fillColor;
  if ( jsonPaint.contains( QStringLiteral( "fill-color" ) ) )
  {
    const QVariant jsonFillColor = jsonPaint.value( QStringLiteral( "fill-color" ) );
    switch ( jsonFillColor.type() )
    {
      case QVariant::Map:
        ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateColorByZoom( jsonFillColor.toMap(), context, &fillColor ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateListByZoom( jsonFillColor.toList(), PropertyType::Color, context, 1, 255, &fillColor ) );
        break;

      case QVariant::String:
        fillColor = parseColor( jsonFillColor.toString(), context );
        break;

      default:
      {
        context.pushWarning( QObject::tr( "Skipping non-implemented color expression" ) );
        break;
      }
    }
  }

  QColor fillOutlineColor;
  if ( !jsonPaint.contains( QStringLiteral( "fill-outline-color" ) ) )
  {
    // fill-outline-color
    if ( fillColor.isValid() )
      fillOutlineColor = fillColor;
    else
    {
      // use fill color data defined property
      if ( ddProperties.isActive( QgsSymbolLayer::PropertyFillColor ) )
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor,  ddProperties.property( QgsSymbolLayer::PropertyFillColor ) );
    }
  }
  else
  {
    const QVariant jsonFillOutlineColor = jsonPaint.value( QStringLiteral( "fill-outline-color" ) );
    switch ( jsonFillOutlineColor.type() )
    {
      case QVariant::Map:
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateColorByZoom( jsonFillOutlineColor.toMap(), context, &fillOutlineColor ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateListByZoom( jsonFillOutlineColor.toList(), PropertyType::Color, context, 1, 255, &fillOutlineColor ) );
        break;

      case QVariant::String:
        fillOutlineColor = parseColor( jsonFillOutlineColor.toString(), context );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented color expression" ) );
        break;
    }
  }

  double fillOpacity = -1.0;
  double rasterOpacity = -1.0;
  if ( jsonPaint.contains( QStringLiteral( "fill-opacity" ) ) )
  {
    const QVariant jsonFillOpacity = jsonPaint.value( QStringLiteral( "fill-opacity" ) );
    switch ( jsonFillOpacity.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        fillOpacity = jsonFillOpacity.toDouble();
        rasterOpacity = fillOpacity;
        break;

      case QVariant::Map:
        if ( ddProperties.isActive( QgsSymbolLayer::PropertyFillColor ) )
        {
          context.pushWarning( QObject::tr( "Could not set opacity of layer %1, opacity already defined in fill color" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
        }
        else
        {
          ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateOpacityByZoom( jsonFillOpacity.toMap(), fillColor.isValid() ? fillColor.alpha() : 255 ) );
          ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateOpacityByZoom( jsonFillOpacity.toMap(), fillOutlineColor.isValid() ? fillOutlineColor.alpha() : 255 ) );
          ddRasterProperties.setProperty( QgsSymbolLayer::PropertyOpacity, parseInterpolateByZoom( jsonFillOpacity.toMap(), context, 100, &rasterOpacity ) );
        }
        break;

      case QVariant::List:
      case QVariant::StringList:
        if ( ddProperties.isActive( QgsSymbolLayer::PropertyFillColor ) )
        {
          context.pushWarning( QObject::tr( "Could not set opacity of layer %1, opacity already defined in fill color" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
        }
        else
        {
          ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateListByZoom( jsonFillOpacity.toList(), PropertyType::Opacity, context, 1, fillColor.isValid() ? fillColor.alpha() : 255 ) );
          ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateListByZoom( jsonFillOpacity.toList(), PropertyType::Opacity, context, 1, fillOutlineColor.isValid() ? fillOutlineColor.alpha() : 255 ) );
          ddRasterProperties.setProperty( QgsSymbolLayer::PropertyOpacity, parseInterpolateListByZoom( jsonFillOpacity.toList(), PropertyType::Numeric, context, 100, 255, nullptr, &rasterOpacity ) );
        }
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented opacity expression" ) );
        break;
    }
  }

  // fill-translate
  QPointF fillTranslate;
  if ( jsonPaint.contains( QStringLiteral( "fill-translate" ) ) )
  {
    const QVariant jsonFillTranslate = jsonPaint.value( QStringLiteral( "fill-translate" ) );
    switch ( jsonFillTranslate.type() )
    {

      case QVariant::Map:
        ddProperties.setProperty( QgsSymbolLayer::PropertyOffset, parseInterpolatePointByZoom( jsonFillTranslate.toMap(), context, context.pixelSizeConversionFactor(), &fillTranslate ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        fillTranslate = QPointF( jsonFillTranslate.toList().value( 0 ).toDouble() * context.pixelSizeConversionFactor(),
                                 jsonFillTranslate.toList().value( 1 ).toDouble() * context.pixelSizeConversionFactor() );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented fill-translate expression" ) );
        break;
    }
  }

  std::unique_ptr< QgsSymbol > symbol( QgsSymbol::defaultSymbol( QgsWkbTypes::PolygonGeometry ) );
  QgsSimpleFillSymbolLayer *fillSymbol = dynamic_cast< QgsSimpleFillSymbolLayer * >( symbol->symbolLayer( 0 ) );

  // set render units
  symbol->setOutputUnit( context.targetUnit() );
  fillSymbol->setOutputUnit( context.targetUnit() );

  if ( !fillTranslate.isNull() )
  {
    fillSymbol->setOffset( fillTranslate );
  }
  fillSymbol->setOffsetUnit( context.targetUnit() );

  if ( jsonPaint.contains( QStringLiteral( "fill-pattern" ) ) )
  {
    // get fill-pattern to set sprite

    const QVariant fillPatternJson = jsonPaint.value( QStringLiteral( "fill-pattern" ) );

    // fill-pattern can be String or Object
    // String: {"fill-pattern": "dash-t"}
    // Object: {"fill-pattern":{"stops":[[11,"wetland8"],[12,"wetland16"]]}}

    switch ( fillPatternJson.type() )
    {
      case QVariant::String:
      {
        QSize spriteSize;
        const QString sprite = retrieveSpriteAsBase64( fillPatternJson.toString(), context, spriteSize );
        if ( !sprite.isEmpty() )
        {
          // when fill-pattern exists, set and insert QgsRasterFillSymbolLayer
          QgsRasterFillSymbolLayer *rasterFill = new QgsRasterFillSymbolLayer();
          rasterFill->setImageFilePath( sprite );
          rasterFill->setCoordinateMode( QgsRasterFillSymbolLayer::Viewport );

          if ( rasterOpacity >= 0 )
            rasterFill->setOpacity( rasterOpacity );
          rasterFill->setDataDefinedProperties( ddRasterProperties );

          symbol->appendSymbolLayer( rasterFill );
        }
        break;
      }

      case QVariant::Map:
      {
#if 0
        // if Object, simpify into one sprite.
        // TODO:
        if isinstance( fill_pattern, dict )
        {
          pattern_stops = fill_pattern.get( "stops", [None] )
                          fill_pattern = pattern_stops[-1][-1]
        }
#endif
        FALLTHROUGH
      }

      default:

        break;
    }
  }

  fillSymbol->setDataDefinedProperties( ddProperties );

  if ( fillOpacity != -1 )
  {
    symbol->setOpacity( fillOpacity );
  }

  if ( fillOutlineColor.isValid() )
  {
    fillSymbol->setStrokeColor( fillOutlineColor );
  }
  else
  {
    fillSymbol->setStrokeStyle( Qt::NoPen );
  }

  if ( fillColor.isValid() )
  {
    fillSymbol->setFillColor( fillColor );
  }
  else
  {
    fillSymbol->setBrushStyle( Qt::NoBrush );
  }

  style.setGeometryType( QgsWkbTypes::PolygonGeometry );
  style.setSymbol( symbol.release() );
  return true;
}

bool QgsMapBoxGlStyleConverter::parseLineLayer( const QVariantMap &jsonLayer, QgsVectorTileBasicRendererStyle &style, QgsMapBoxGlStyleConversionContext &context )
{
  if ( !jsonLayer.contains( QStringLiteral( "paint" ) ) )
  {
    context.pushWarning( QObject::tr( "Style layer %1 has no paint property, skipping" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
    return false;
  }

  const QVariantMap jsonPaint = jsonLayer.value( QStringLiteral( "paint" ) ).toMap();

  QgsPropertyCollection ddProperties;

  // line color
  QColor lineColor;
  if ( jsonPaint.contains( QStringLiteral( "line-color" ) ) )
  {
    const QVariant jsonLineColor = jsonPaint.value( QStringLiteral( "line-color" ) );
    switch ( jsonLineColor.type() )
    {
      case QVariant::Map:
        ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateColorByZoom( jsonLineColor.toMap(), context, &lineColor ) );
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, ddProperties.property( QgsSymbolLayer::PropertyFillColor ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddProperties.setProperty( QgsSymbolLayer::PropertyFillColor, parseInterpolateListByZoom( jsonLineColor.toList(), PropertyType::Color, context, 1, 255, &lineColor ) );
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, ddProperties.property( QgsSymbolLayer::PropertyFillColor ) );
        break;

      case QVariant::String:
        lineColor = parseColor( jsonLineColor.toString(), context );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented color expression" ) );
        break;
    }
  }


  double lineWidth = 1.0;
  if ( jsonPaint.contains( QStringLiteral( "line-width" ) ) )
  {
    const QVariant jsonLineWidth = jsonPaint.value( QStringLiteral( "line-width" ) );
    switch ( jsonLineWidth.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        lineWidth = jsonLineWidth.toDouble() * context.pixelSizeConversionFactor();
        break;

      case QVariant::Map:
        lineWidth = -1;
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeWidth, parseInterpolateByZoom( jsonLineWidth.toMap(), context, context.pixelSizeConversionFactor(), &lineWidth ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeWidth, parseInterpolateListByZoom( jsonLineWidth.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &lineWidth ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented line-width expression" ) );
        break;
    }
  }

  double lineOffset = 0.0;
  if ( jsonPaint.contains( QStringLiteral( "line-offset" ) ) )
  {
    const QVariant jsonLineOffset = jsonPaint.value( QStringLiteral( "line-offset" ) );
    switch ( jsonLineOffset.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        lineOffset = -jsonLineOffset.toDouble() * context.pixelSizeConversionFactor();
        break;

      case QVariant::Map:
        lineWidth = -1;
        ddProperties.setProperty( QgsSymbolLayer::PropertyOffset, parseInterpolateByZoom( jsonLineOffset.toMap(), context, context.pixelSizeConversionFactor() * -1, &lineOffset ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddProperties.setProperty( QgsSymbolLayer::PropertyOffset, parseInterpolateListByZoom( jsonLineOffset.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor() * -1, 255, nullptr, &lineOffset ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented line-offset expression" ) );
        break;
    }
  }

  double lineOpacity = -1.0;
  if ( jsonPaint.contains( QStringLiteral( "line-opacity" ) ) )
  {
    const QVariant jsonLineOpacity = jsonPaint.value( QStringLiteral( "line-opacity" ) );
    switch ( jsonLineOpacity.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        lineOpacity = jsonLineOpacity.toDouble();
        break;

      case QVariant::Map:
        if ( ddProperties.isActive( QgsSymbolLayer::PropertyStrokeColor ) )
        {
          context.pushWarning( QObject::tr( "Could not set opacity of layer %1, opacity already defined in stroke color" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
        }
        else
        {
          ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateOpacityByZoom( jsonLineOpacity.toMap(), lineColor.isValid() ? lineColor.alpha() : 255 ) );
        }
        break;

      case QVariant::List:
      case QVariant::StringList:
        if ( ddProperties.isActive( QgsSymbolLayer::PropertyStrokeColor ) )
        {
          context.pushWarning( QObject::tr( "Could not set opacity of layer %1, opacity already defined in stroke color" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
        }
        else
        {
          ddProperties.setProperty( QgsSymbolLayer::PropertyStrokeColor, parseInterpolateListByZoom( jsonLineOpacity.toList(), PropertyType::Opacity, context, 1, lineColor.isValid() ? lineColor.alpha() : 255 ) );
        }
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented opacity expression" ) );
        break;
    }
  }

  QVector< double > dashVector;
  if ( jsonPaint.contains( QStringLiteral( "line-dasharray" ) ) )
  {
    const QVariant jsonLineDashArray = jsonPaint.value( QStringLiteral( "line-dasharray" ) );
    switch ( jsonLineDashArray.type() )
    {
      case QVariant::Map:
      {
        //TODO improve parsing (use PropertyCustomDash?)
        const QVariantList dashSource = jsonLineDashArray.toMap().value( QStringLiteral( "stops" ) ).toList().last().toList().value( 1 ).toList();
        for ( const QVariant &v : dashSource )
        {
          dashVector << v.toDouble() * context.pixelSizeConversionFactor();
        }
        break;
      }

      case QVariant::List:
      case QVariant::StringList:
      {
        const QVariantList dashSource = jsonLineDashArray.toList();
        for ( const QVariant &v : dashSource )
        {
          dashVector << v.toDouble() * context.pixelSizeConversionFactor();
        }
        break;
      }

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented dash vector expression" ) );
        break;
    }
  }

  Qt::PenCapStyle penCapStyle = Qt::FlatCap;
  Qt::PenJoinStyle penJoinStyle = Qt::MiterJoin;
  if ( jsonLayer.contains( QStringLiteral( "layout" ) ) )
  {
    const QVariantMap jsonLayout = jsonLayer.value( QStringLiteral( "layout" ) ).toMap();
    if ( jsonLayout.contains( QStringLiteral( "line-cap" ) ) )
    {
      penCapStyle = parseCapStyle( jsonLayout.value( QStringLiteral( "line-cap" ) ).toString() );
    }
    if ( jsonLayout.contains( QStringLiteral( "line-join" ) ) )
    {
      penJoinStyle = parseJoinStyle( jsonLayout.value( QStringLiteral( "line-join" ) ).toString() );
    }
  }

  std::unique_ptr< QgsSymbol > symbol( QgsSymbol::defaultSymbol( QgsWkbTypes::LineGeometry ) );
  QgsSimpleLineSymbolLayer *lineSymbol = dynamic_cast< QgsSimpleLineSymbolLayer * >( symbol->symbolLayer( 0 ) );

  // set render units
  symbol->setOutputUnit( context.targetUnit() );
  lineSymbol->setOutputUnit( context.targetUnit() );
  lineSymbol->setPenCapStyle( penCapStyle );
  lineSymbol->setPenJoinStyle( penJoinStyle );
  lineSymbol->setDataDefinedProperties( ddProperties );
  lineSymbol->setOffset( lineOffset );
  lineSymbol->setOffsetUnit( context.targetUnit() );

  if ( lineOpacity != -1 )
  {
    symbol->setOpacity( lineOpacity );
  }
  if ( lineColor.isValid() )
  {
    lineSymbol->setColor( lineColor );
  }
  if ( lineWidth != -1 )
  {
    lineSymbol->setWidth( lineWidth );
  }
  if ( !dashVector.empty() )
  {
    lineSymbol->setUseCustomDashPattern( true );
    lineSymbol->setCustomDashVector( dashVector );
  }

  style.setGeometryType( QgsWkbTypes::LineGeometry );
  style.setSymbol( symbol.release() );
  return true;
}

void QgsMapBoxGlStyleConverter::parseSymbolLayer( const QVariantMap &jsonLayer, QgsVectorTileBasicRendererStyle &renderer, bool &hasRenderer, QgsVectorTileBasicLabelingStyle &labelingStyle, bool &hasLabeling, QgsMapBoxGlStyleConversionContext &context )
{
  hasLabeling = false;
  hasRenderer = false;

  if ( !jsonLayer.contains( QStringLiteral( "layout" ) ) )
  {
    context.pushWarning( QObject::tr( "Style layer %1 has no layout property, skipping" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
    return;
  }
  const QVariantMap jsonLayout = jsonLayer.value( QStringLiteral( "layout" ) ).toMap();
  if ( !jsonLayout.contains( QStringLiteral( "text-field" ) ) )
  {
    hasRenderer = parseSymbolLayerAsRenderer( jsonLayer, renderer, context );
    return;
  }

  if ( !jsonLayer.contains( QStringLiteral( "paint" ) ) )
  {
    context.pushWarning( QObject::tr( "Style layer %1 has no paint property, skipping" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
    return;
  }
  const QVariantMap jsonPaint = jsonLayer.value( QStringLiteral( "paint" ) ).toMap();

  QgsPropertyCollection ddLabelProperties;

  double textSize = 16.0;
  if ( jsonLayout.contains( QStringLiteral( "text-size" ) ) )
  {
    const QVariant jsonTextSize = jsonLayout.value( QStringLiteral( "text-size" ) );
    switch ( jsonTextSize.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        textSize = jsonTextSize.toDouble() * context.pixelSizeConversionFactor();
        break;

      case QVariant::Map:
        textSize = -1;
        ddLabelProperties.setProperty( QgsPalLayerSettings::Size, parseInterpolateByZoom( jsonTextSize.toMap(), context, context.pixelSizeConversionFactor(), &textSize ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        textSize = -1;
        ddLabelProperties.setProperty( QgsPalLayerSettings::Size, parseInterpolateListByZoom( jsonTextSize.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &textSize ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-size expression" ) );
        break;
    }
  }

  // em to character tends to underestimate, so scale up by ~40%
  constexpr double EM_TO_CHARS = 1.4;

  double textMaxWidth = -1;
  if ( jsonLayout.contains( QStringLiteral( "text-max-width" ) ) )
  {
    const QVariant jsonTextMaxWidth = jsonLayout.value( QStringLiteral( "text-max-width" ) );
    switch ( jsonTextMaxWidth.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        textMaxWidth = jsonTextMaxWidth.toDouble() * EM_TO_CHARS;
        break;

      case QVariant::Map:
        ddLabelProperties.setProperty( QgsPalLayerSettings::AutoWrapLength, parseInterpolateByZoom( jsonTextMaxWidth.toMap(), context, EM_TO_CHARS, &textMaxWidth ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddLabelProperties.setProperty( QgsPalLayerSettings::AutoWrapLength, parseInterpolateListByZoom( jsonTextMaxWidth.toList(), PropertyType::Numeric, context, EM_TO_CHARS, 255, nullptr, &textMaxWidth ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-max-width expression" ) );
        break;
    }
  }

  double textLetterSpacing = -1;
  if ( jsonLayout.contains( QStringLiteral( "text-letter-spacing" ) ) )
  {
    const QVariant jsonTextLetterSpacing = jsonLayout.value( QStringLiteral( "text-letter-spacing" ) );
    switch ( jsonTextLetterSpacing.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        textLetterSpacing = jsonTextLetterSpacing.toDouble();
        break;

      case QVariant::Map:
        ddLabelProperties.setProperty( QgsPalLayerSettings::FontLetterSpacing, parseInterpolateByZoom( jsonTextLetterSpacing.toMap(), context, 1, &textLetterSpacing ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddLabelProperties.setProperty( QgsPalLayerSettings::FontLetterSpacing, parseInterpolateListByZoom( jsonTextLetterSpacing.toList(), PropertyType::Numeric, context, 1, 255, nullptr, &textLetterSpacing ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-letter-spacing expression" ) );
        break;
    }
  }

  QFont textFont;
  bool foundFont = false;
  if ( jsonLayout.contains( QStringLiteral( "text-font" ) ) )
  {
    const QVariant jsonTextFont = jsonLayout.value( QStringLiteral( "text-font" ) );
    if ( jsonTextFont.type() != QVariant::List && jsonTextFont.type() != QVariant::StringList && jsonTextFont.type() != QVariant::String )
    {
      context.pushWarning( QObject::tr( "Skipping non-implemented text-font expression" ) );
    }
    else
    {
      QString fontName;
      switch ( jsonTextFont.type() )
      {
        case QVariant::List:
        case QVariant::StringList:
          fontName = jsonTextFont.toList().value( 0 ).toString();
          break;

        case QVariant::String:
          fontName = jsonTextFont.toString();
          break;

        default:
          break;
      }

      const QStringList textFontParts = fontName.split( ' ' );
      for ( int i = 1; i < textFontParts.size(); ++i )
      {
        const QString candidateFontName = textFontParts.mid( 0, i ).join( ' ' );
        const QString candidateFontStyle = textFontParts.mid( i ).join( ' ' );
        if ( QgsFontUtils::fontFamilyHasStyle( candidateFontName, candidateFontStyle ) )
        {
          textFont = QFont( candidateFontName );
          textFont.setStyleName( candidateFontStyle );
          foundFont = true;
          break;
        }
      }

      if ( !foundFont )
      {
        // probably won't work, but we'll try anyway... maybe the json isn't following the spec correctly!!
        textFont = QFont( fontName );
        foundFont = true;
      }
    }
  }

  // text color
  QColor textColor;
  if ( jsonPaint.contains( QStringLiteral( "text-color" ) ) )
  {
    const QVariant jsonTextColor = jsonPaint.value( QStringLiteral( "text-color" ) );
    switch ( jsonTextColor.type() )
    {
      case QVariant::Map:
        ddLabelProperties.setProperty( QgsPalLayerSettings::Color, parseInterpolateColorByZoom( jsonTextColor.toMap(), context, &textColor ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddLabelProperties.setProperty( QgsPalLayerSettings::Color, parseInterpolateListByZoom( jsonTextColor.toList(), PropertyType::Color, context, 1, 255, &textColor ) );
        break;

      case QVariant::String:
        textColor = parseColor( jsonTextColor.toString(), context );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-color expression" ) );
        break;
    }
  }

  // buffer color
  QColor bufferColor;
  if ( jsonPaint.contains( QStringLiteral( "text-halo-color" ) ) )
  {
    const QVariant jsonBufferColor = jsonPaint.value( QStringLiteral( "text-halo-color" ) );
    switch ( jsonBufferColor.type() )
    {
      case QVariant::Map:
        ddLabelProperties.setProperty( QgsPalLayerSettings::BufferColor, parseInterpolateColorByZoom( jsonBufferColor.toMap(), context, &bufferColor ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        ddLabelProperties.setProperty( QgsPalLayerSettings::BufferColor, parseInterpolateListByZoom( jsonBufferColor.toList(), PropertyType::Color, context, 1, 255, &bufferColor ) );
        break;

      case QVariant::String:
        bufferColor = parseColor( jsonBufferColor.toString(), context );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-halo-color expression" ) );
        break;
    }
  }

  double bufferSize = 0.0;
  if ( jsonPaint.contains( QStringLiteral( "text-halo-width" ) ) )
  {
    const QVariant jsonHaloWidth = jsonPaint.value( QStringLiteral( "text-halo-width" ) );
    switch ( jsonHaloWidth.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
        bufferSize = jsonHaloWidth.toDouble() * context.pixelSizeConversionFactor();
        break;

      case QVariant::Map:
        bufferSize = 1;
        ddLabelProperties.setProperty( QgsPalLayerSettings::BufferSize, parseInterpolateByZoom( jsonHaloWidth.toMap(), context, context.pixelSizeConversionFactor(), &bufferSize ) );
        break;

      case QVariant::List:
      case QVariant::StringList:
        bufferSize = 1;
        ddLabelProperties.setProperty( QgsPalLayerSettings::BufferSize, parseInterpolateListByZoom( jsonHaloWidth.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &bufferSize ) );
        break;

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-halo-width expression" ) );
        break;
    }
  }

  double haloBlurSize = 0;
  if ( jsonPaint.contains( QStringLiteral( "text-halo-blur" ) ) )
  {
    const QVariant jsonTextHaloBlur = jsonPaint.value( QStringLiteral( "text-halo-blur" ) );
    switch ( jsonTextHaloBlur.type() )
    {
      case QVariant::Int:
      case QVariant::Double:
      {
        haloBlurSize = jsonTextHaloBlur.toDouble() * context.pixelSizeConversionFactor();
        break;
      }

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-halo-width expression" ) );
        break;
    }
  }

  QgsTextFormat format;
  format.setSizeUnit( context.targetUnit() );
  if ( textColor.isValid() )
    format.setColor( textColor );
  if ( textSize >= 0 )
    format.setSize( textSize );
  if ( foundFont )
    format.setFont( textFont );
  if ( textLetterSpacing > 0 )
  {
    QFont f = format.font();
    f.setLetterSpacing( QFont::AbsoluteSpacing, textLetterSpacing );
    format.setFont( f );
  }

  if ( bufferSize > 0 )
  {
    format.buffer().setEnabled( true );
    format.buffer().setSize( bufferSize );
    format.buffer().setSizeUnit( context.targetUnit() );
    format.buffer().setColor( bufferColor );

    if ( haloBlurSize > 0 )
    {
      QgsEffectStack *stack = new QgsEffectStack();
      QgsBlurEffect *blur = new QgsBlurEffect() ;
      blur->setEnabled( true );
      blur->setBlurUnit( context.targetUnit() );
      blur->setBlurLevel( haloBlurSize );
      blur->setBlurMethod( QgsBlurEffect::StackBlur );
      stack->appendEffect( blur );
      stack->setEnabled( true );
      format.buffer().setPaintEffect( stack );
    }
  }

  QgsPalLayerSettings labelSettings;

  if ( textMaxWidth > 0 )
  {
    labelSettings.autoWrapLength = textMaxWidth;
  }

  // convert field name

  auto processLabelField = []( const QString & string, bool & isExpression )->QString
  {
    // {field_name} is permitted in string -- if multiple fields are present, convert them to an expression
    // but if single field is covered in {}, return it directly
    const QRegularExpression singleFieldRx( QStringLiteral( "^{([^}]+)}$" ) );
    QRegularExpressionMatch match = singleFieldRx.match( string );
    if ( match.hasMatch() )
    {
      isExpression = false;
      return match.captured( 1 );
    }

    const QRegularExpression multiFieldRx( QStringLiteral( "(?={[^}]+})" ) );
    const QStringList parts = string.split( multiFieldRx );
    if ( parts.size() > 1 )
    {
      isExpression = true;

      QStringList res;
      for ( const QString &part : parts )
      {
        if ( part.isEmpty() )
          continue;

        // part will start at a {field} reference
        const QStringList split = part.split( '}' );
        res << QgsExpression::quotedColumnRef( split.at( 0 ).mid( 1 ) );
        if ( !split.at( 1 ).isEmpty() )
          res << QgsExpression::quotedValue( split.at( 1 ) );
      }
      return QStringLiteral( "concat(%1)" ).arg( res.join( ',' ) );
    }
    else
    {
      isExpression = false;
      return string;
    }
  };

  if ( jsonLayout.contains( QStringLiteral( "text-field" ) ) )
  {
    const QVariant jsonTextField = jsonLayout.value( QStringLiteral( "text-field" ) );
    switch ( jsonTextField.type() )
    {
      case QVariant::String:
      {
        labelSettings.fieldName = processLabelField( jsonTextField.toString(), labelSettings.isExpression );
        break;
      }

      case QVariant::List:
      case QVariant::StringList:
      {
        const QVariantList textFieldList = jsonTextField.toList();
        /*
         * e.g.
         *     "text-field": ["format",
         *                    "foo", { "font-scale": 1.2 },
         *                    "bar", { "font-scale": 0.8 }
         * ]
         */
        if ( textFieldList.size() > 2 && textFieldList.at( 0 ).toString() == QLatin1String( "format" ) )
        {
          QStringList parts;
          for ( int i = 1; i < textFieldList.size(); ++i )
          {
            bool isExpression = false;
            const QString part = processLabelField( textFieldList.at( i ).toString(), isExpression );
            if ( !isExpression )
              parts << QgsExpression::quotedColumnRef( part );
            else
              parts << part;
            // TODO -- we could also translate font color, underline, overline, strikethrough to HTML tags!
            i += 1;
          }
          labelSettings.fieldName = QStringLiteral( "concat(%1)" ).arg( parts.join( ',' ) );
          labelSettings.isExpression = true;
        }
        else
        {
          /*
           * e.g.
           *     "text-field": ["to-string", ["get", "name"]]
           */
          labelSettings.fieldName = parseExpression( textFieldList, context );
          labelSettings.isExpression = true;
        }
        break;
      }

      default:
        context.pushWarning( QObject::tr( "Skipping non-implemented text-field expression" ) );
        break;
    }
  }

  if ( jsonLayout.contains( QStringLiteral( "text-transform" ) ) )
  {
    const QString textTransform = jsonLayout.value( QStringLiteral( "text-transform" ) ).toString();
    if ( textTransform == QLatin1String( "uppercase" ) )
    {
      labelSettings.fieldName = QStringLiteral( "upper(%1)" ).arg( labelSettings.isExpression ? labelSettings.fieldName : QgsExpression::quotedColumnRef( labelSettings.fieldName ) );
    }
    else if ( textTransform == QLatin1String( "lowercase" ) )
    {
      labelSettings.fieldName = QStringLiteral( "lower(%1)" ).arg( labelSettings.isExpression ? labelSettings.fieldName : QgsExpression::quotedColumnRef( labelSettings.fieldName ) );
    }
    labelSettings.isExpression = true;
  }

  labelSettings.placement = QgsPalLayerSettings::OverPoint;
  QgsWkbTypes::GeometryType geometryType = QgsWkbTypes::PointGeometry;
  if ( jsonLayout.contains( QStringLiteral( "symbol-placement" ) ) )
  {
    const QString symbolPlacement = jsonLayout.value( QStringLiteral( "symbol-placement" ) ).toString();
    if ( symbolPlacement == QLatin1String( "line" ) )
    {
      labelSettings.placement = QgsPalLayerSettings::Curved;
      labelSettings.lineSettings().setPlacementFlags( QgsLabeling::OnLine );
      geometryType = QgsWkbTypes::LineGeometry;
    }
  }

  if ( labelSettings.placement == QgsPalLayerSettings::OverPoint )
  {
    if ( jsonLayout.contains( QStringLiteral( "text-anchor" ) ) )
    {
      const QVariant jsonTextAnchor = jsonLayout.value( QStringLiteral( "text-anchor" ) );
      QString textAnchor;

      const QVariantMap conversionMap
      {
        { QStringLiteral( "center" ), 4 },
        { QStringLiteral( "left" ), 5 },
        { QStringLiteral( "right" ), 3 },
        { QStringLiteral( "top" ), 7 },
        { QStringLiteral( "bottom" ), 1 },
        { QStringLiteral( "top-left" ), 8 },
        { QStringLiteral( "top-right" ), 6 },
        { QStringLiteral( "bottom-left" ), 2 },
        { QStringLiteral( "bottom-right" ), 0 },
      };

      switch ( jsonTextAnchor.type() )
      {
        case QVariant::String:
          textAnchor = jsonTextAnchor.toString();
          break;

        case QVariant::List:
          ddLabelProperties.setProperty( QgsPalLayerSettings::OffsetQuad, QgsProperty::fromExpression( parseStringStops( jsonTextAnchor.toList(), context, conversionMap, &textAnchor ) ) );
          break;

        case QVariant::Map:
          ddLabelProperties.setProperty( QgsPalLayerSettings::OffsetQuad, parseInterpolateStringByZoom( jsonTextAnchor.toMap(), context, conversionMap, &textAnchor ) );
          break;

        default:
          context.pushWarning( QObject::tr( "Skipping non-implemented text-anchor expression" ) );
          break;
      }

      if ( textAnchor == QLatin1String( "center" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantOver;
      else if ( textAnchor == QLatin1String( "left" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantRight;
      else if ( textAnchor == QLatin1String( "right" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantLeft;
      else if ( textAnchor == QLatin1String( "top" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantBelow;
      else if ( textAnchor == QLatin1String( "bottom" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantAbove;
      else if ( textAnchor == QLatin1String( "top-left" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantBelowRight;
      else if ( textAnchor == QLatin1String( "top-right" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantBelowLeft;
      else if ( textAnchor == QLatin1String( "bottom-left" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantAboveRight;
      else if ( textAnchor == QLatin1String( "bottom-right" ) )
        labelSettings.quadOffset = QgsPalLayerSettings::QuadrantAboveLeft;
    }

    QPointF textOffset;
    if ( jsonLayout.contains( QStringLiteral( "text-offset" ) ) )
    {
      const QVariant jsonTextOffset = jsonLayout.value( QStringLiteral( "text-offset" ) );

      // units are ems!
      switch ( jsonTextOffset.type() )
      {
        case QVariant::Map:
          ddLabelProperties.setProperty( QgsPalLayerSettings::OffsetXY, parseInterpolatePointByZoom( jsonTextOffset.toMap(), context, textSize, &textOffset ) );
          break;

        case QVariant::List:
        case QVariant::StringList:
          textOffset = QPointF( jsonTextOffset.toList().value( 0 ).toDouble() * textSize,
                                jsonTextOffset.toList().value( 1 ).toDouble() * textSize );
          break;

        default:
          context.pushWarning( QObject::tr( "Skipping non-implemented fill-translate expression" ) );
          break;
      }

      if ( !textOffset.isNull() )
      {
        labelSettings.offsetUnits = context.targetUnit();
        labelSettings.xOffset = textOffset.x();
        labelSettings.yOffset = textOffset.y();
      }
    }
  }

  if ( textSize >= 0 )
  {
    // TODO -- this probably needs revisiting -- it was copied from the MapTiler code, but may be wrong...
    labelSettings.priority = std::min( textSize / ( context.pixelSizeConversionFactor() * 3 ), 10.0 );
  }

  labelSettings.setFormat( format );

  // use a low obstacle weight for layers by default -- we'd rather have more labels for these layers, even if placement isn't ideal
  labelSettings.obstacleSettings().setFactor( 0.1 );

  labelSettings.setDataDefinedProperties( ddLabelProperties );

  labelingStyle.setGeometryType( geometryType );
  labelingStyle.setLabelSettings( labelSettings );

  hasLabeling = true;

  if ( jsonLayout.contains( QStringLiteral( "icon-image" ) ) )
  {
    QSize spriteSize;
    const QString sprite = retrieveSpriteAsBase64( jsonLayout.value( QStringLiteral( "icon-image" ) ).toString(), context, spriteSize );
    if ( !sprite.isEmpty() )
    {
      hasRenderer = true;
      QgsRasterMarkerSymbolLayer *rasterMarker = new QgsRasterMarkerSymbolLayer( );
      rasterMarker->setPath( sprite );
      rasterMarker->setSize( context.pixelSizeConversionFactor() * spriteSize.width() );
      rasterMarker->setSizeUnit( context.targetUnit() );

      QgsPropertyCollection markerDdProperties;
      double rotation = 0.0;
      if ( jsonLayout.contains( QStringLiteral( "icon-rotate" ) ) )
      {
        const QVariant jsonIconRotate = jsonLayout.value( QStringLiteral( "icon-rotate" ) );
        switch ( jsonIconRotate.type() )
        {
          case QVariant::Int:
          case QVariant::Double:
            rotation = jsonIconRotate.toDouble();
            break;

          case QVariant::Map:
            markerDdProperties.setProperty( QgsSymbolLayer::PropertyAngle, parseInterpolateByZoom( jsonIconRotate.toMap(), context, context.pixelSizeConversionFactor(), &rotation ) );
            break;

          case QVariant::List:
          case QVariant::StringList:
            markerDdProperties.setProperty( QgsSymbolLayer::PropertyAngle, parseInterpolateListByZoom( jsonIconRotate.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &rotation ) );
            break;

          default:
            context.pushWarning( QObject::tr( "Skipping non-implemented icon-rotate expression" ) );
            break;
        }
      }

      double iconOpacity = -1.0;
      if ( jsonPaint.contains( QStringLiteral( "icon-opacity" ) ) )
      {
        const QVariant jsonIconOpacity = jsonPaint.value( QStringLiteral( "icon-opacity" ) );
        switch ( jsonIconOpacity.type() )
        {
          case QVariant::Int:
          case QVariant::Double:
            iconOpacity = jsonIconOpacity.toDouble();
            break;

          case QVariant::Map:
            markerDdProperties.setProperty( QgsSymbolLayer::PropertyOpacity, parseInterpolateByZoom( jsonIconOpacity.toMap(), context, 100, &iconOpacity ) );
            break;

          case QVariant::List:
          case QVariant::StringList:
            markerDdProperties.setProperty( QgsSymbolLayer::PropertyOpacity, parseInterpolateListByZoom( jsonIconOpacity.toList(), PropertyType::Numeric, context, 100, 255, nullptr, &iconOpacity ) );
            break;

          default:
            context.pushWarning( QObject::tr( "Skipping non-implemented icon-opacity expression" ) );
            break;
        }
      }

      rasterMarker->setDataDefinedProperties( markerDdProperties );
      rasterMarker->setAngle( rotation );
      if ( iconOpacity >= 0 )
        rasterMarker->setOpacity( iconOpacity );

      QgsMarkerSymbol *markerSymbol = new QgsMarkerSymbol( QgsSymbolLayerList() << rasterMarker );
      renderer.setSymbol( markerSymbol );
      renderer.setGeometryType( QgsWkbTypes::PointGeometry );
    }
  }
}

bool QgsMapBoxGlStyleConverter::parseSymbolLayerAsRenderer( const QVariantMap &jsonLayer, QgsVectorTileBasicRendererStyle &rendererStyle, QgsMapBoxGlStyleConversionContext &context )
{
  if ( !jsonLayer.contains( QStringLiteral( "layout" ) ) )
  {
    context.pushWarning( QObject::tr( "Style layer %1 has no layout property, skipping" ).arg( jsonLayer.value( QStringLiteral( "id" ) ).toString() ) );
    return false;
  }
  const QVariantMap jsonLayout = jsonLayer.value( QStringLiteral( "layout" ) ).toMap();

  if ( jsonLayout.value( QStringLiteral( "symbol-placement" ) ).toString() == QLatin1String( "line" ) )
  {
    QgsPropertyCollection ddProperties;

    double spacing = -1.0;
    if ( jsonLayout.contains( QStringLiteral( "symbol-spacing" ) ) )
    {
      const QVariant jsonSpacing = jsonLayout.value( QStringLiteral( "symbol-spacing" ) );
      switch ( jsonSpacing.type() )
      {
        case QVariant::Int:
        case QVariant::Double:
          spacing = jsonSpacing.toDouble() * context.pixelSizeConversionFactor();
          break;

        case QVariant::Map:
          ddProperties.setProperty( QgsSymbolLayer::PropertyInterval, parseInterpolateByZoom( jsonSpacing.toMap(), context, context.pixelSizeConversionFactor(), &spacing ) );
          break;

        case QVariant::List:
        case QVariant::StringList:
          ddProperties.setProperty( QgsSymbolLayer::PropertyInterval, parseInterpolateListByZoom( jsonSpacing.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &spacing ) );
          break;

        default:
          context.pushWarning( QObject::tr( "Skipping non-implemented symbol-spacing expression" ) );
          break;
      }
    }

    bool rotateMarkers = true;
    if ( jsonLayout.contains( QStringLiteral( "icon-rotation-alignment" ) ) )
    {
      const QString alignment = jsonLayout.value( QStringLiteral( "icon-rotation-alignment" ) ).toString();
      if ( alignment == QLatin1String( "map" ) || alignment == QLatin1String( "auto" ) )
      {
        rotateMarkers = true;
      }
      else if ( alignment == QLatin1String( "viewport" ) )
      {
        rotateMarkers = false;
      }
    }

    QgsPropertyCollection markerDdProperties;
    double rotation = 0.0;
    if ( jsonLayout.contains( QStringLiteral( "icon-rotate" ) ) )
    {
      const QVariant jsonIconRotate = jsonLayout.value( QStringLiteral( "icon-rotate" ) );
      switch ( jsonIconRotate.type() )
      {
        case QVariant::Int:
        case QVariant::Double:
          rotation = jsonIconRotate.toDouble();
          break;

        case QVariant::Map:
          markerDdProperties.setProperty( QgsSymbolLayer::PropertyAngle, parseInterpolateByZoom( jsonIconRotate.toMap(), context, context.pixelSizeConversionFactor(), &rotation ) );
          break;

        case QVariant::List:
        case QVariant::StringList:
          markerDdProperties.setProperty( QgsSymbolLayer::PropertyAngle, parseInterpolateListByZoom( jsonIconRotate.toList(), PropertyType::Numeric, context, context.pixelSizeConversionFactor(), 255, nullptr, &rotation ) );
          break;

        default:
          context.pushWarning( QObject::tr( "Skipping non-implemented icon-rotate expression" ) );
          break;
      }
    }

    QgsMarkerLineSymbolLayer *lineSymbol = new QgsMarkerLineSymbolLayer( rotateMarkers, spacing > 0 ? spacing : 1 );
    lineSymbol->setOutputUnit( context.targetUnit() );
    lineSymbol->setDataDefinedProperties( ddProperties );
    if ( spacing <= 0 )
    {
      // if spacing isn't specified, it's a central point marker only
      lineSymbol->setPlacement( QgsTemplatedLineSymbolLayerBase::CentralPoint );
    }

    QgsRasterMarkerSymbolLayer *markerLayer = new QgsRasterMarkerSymbolLayer( );
    QSize spriteSize;
    const QString sprite = retrieveSpriteAsBase64( jsonLayout.value( QStringLiteral( "icon-image" ) ).toString(), context, spriteSize );
    if ( !sprite.isNull() )
    {
      markerLayer->setPath( sprite );
      markerLayer->setSize( context.pixelSizeConversionFactor() * spriteSize.width() );
      markerLayer->setSizeUnit( context.targetUnit() );
    }

    markerLayer->setDataDefinedProperties( markerDdProperties );
    markerLayer->setAngle( rotation );
    lineSymbol->setSubSymbol( new QgsMarkerSymbol( QgsSymbolLayerList() << markerLayer ) );

    std::unique_ptr< QgsSymbol > symbol = qgis::make_unique< QgsLineSymbol >( QgsSymbolLayerList() << lineSymbol );

    // set render units
    symbol->setOutputUnit( context.targetUnit() );
    lineSymbol->setOutputUnit( context.targetUnit() );

    rendererStyle.setGeometryType( QgsWkbTypes::LineGeometry );
    rendererStyle.setSymbol( symbol.release() );
    return true;
  }

  return false;
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolateColorByZoom( const QVariantMap &json, QgsMapBoxGlStyleConversionContext &context, QColor *defaultColor )
{
  const double base = json.value( QStringLiteral( "base" ), QStringLiteral( "1" ) ).toDouble();
  const QVariantList stops = json.value( QStringLiteral( "stops" ) ).toList();
  if ( stops.empty() )
    return QgsProperty();

  QString caseString = QStringLiteral( "CASE " );

  for ( int i = 0; i < stops.length() - 1; ++i )
  {
    // step bottom zoom
    const QString bz = stops.at( i ).toList().value( 0 ).toString();
    // step top zoom
    const QString tz = stops.at( i + 1 ).toList().value( 0 ).toString();

    const QColor bottomColor = parseColor( stops.at( i ).toList().value( 1 ), context );
    const QColor topColor = parseColor( stops.at( i + 1 ).toList().value( 1 ), context );

    int bcHue;
    int bcSat;
    int bcLight;
    int bcAlpha;
    colorAsHslaComponents( bottomColor, bcHue, bcSat, bcLight, bcAlpha );
    int tcHue;
    int tcSat;
    int tcLight;
    int tcAlpha;
    colorAsHslaComponents( topColor, tcHue, tcSat, tcLight, tcAlpha );

    caseString += QStringLiteral( "WHEN @zoom_level >= %1 AND @zoom_level < %2 THEN color_hsla("
                                  "%3, %4, %5, %6) " ).arg( bz, tz,
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bcHue, tcHue, base ),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bcSat, tcSat, base ),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bcLight, tcLight, base ),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bcAlpha, tcAlpha, base ) );
  }

  // top color
  const QString tz = stops.last().toList().value( 0 ).toString();
  const QColor topColor = parseColor( stops.last().toList().value( 1 ), context );
  int tcHue;
  int tcSat;
  int tcLight;
  int tcAlpha;
  colorAsHslaComponents( topColor, tcHue, tcSat, tcLight, tcAlpha );

  caseString += QStringLiteral( "WHEN @zoom_level >= %1 THEN color_hsla(%2, %3, %4, %5) "
                                "ELSE color_hsla(%2, %3, %4, %5) END" ).arg( tz )
                .arg( tcHue ).arg( tcSat ).arg( tcLight ).arg( tcAlpha );


  if ( !stops.empty() && defaultColor )
    *defaultColor = parseColor( stops.value( 0 ).toList().value( 1 ).toString(), context );

  return QgsProperty::fromExpression( caseString );
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolateByZoom( const QVariantMap &json, QgsMapBoxGlStyleConversionContext &context, double multiplier, double *defaultNumber )
{
  const double base = json.value( QStringLiteral( "base" ), QStringLiteral( "1" ) ).toDouble();
  const QVariantList stops = json.value( QStringLiteral( "stops" ) ).toList();
  if ( stops.empty() )
    return QgsProperty();

  QString scaleExpression;
  if ( stops.size() <= 2 )
  {
    scaleExpression = interpolateExpression( stops.value( 0 ).toList().value( 0 ).toDouble(),
                      stops.last().toList().value( 0 ).toDouble(),
                      stops.value( 0 ).toList().value( 1 ).toDouble(),
                      stops.last().toList().value( 1 ).toDouble(), base, multiplier );
  }
  else
  {
    scaleExpression = parseStops( base, stops, multiplier, context );
  }

  if ( !stops.empty() && defaultNumber )
    *defaultNumber = stops.value( 0 ).toList().value( 1 ).toDouble() * multiplier;

  return QgsProperty::fromExpression( scaleExpression );
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolateOpacityByZoom( const QVariantMap &json, int maxOpacity )
{
  const double base = json.value( QStringLiteral( "base" ), QStringLiteral( "1" ) ).toDouble();
  const QVariantList stops = json.value( QStringLiteral( "stops" ) ).toList();
  if ( stops.empty() )
    return QgsProperty();

  QString scaleExpression;
  if ( stops.length() <= 2 )
  {
    scaleExpression = QStringLiteral( "set_color_part(@symbol_color, 'alpha', %1)" )
                      .arg( interpolateExpression( stops.value( 0 ).toList().value( 0 ).toDouble(),
                            stops.last().toList().value( 0 ).toDouble(),
                            stops.value( 0 ).toList().value( 1 ).toDouble() * maxOpacity,
                            stops.last().toList().value( 1 ).toDouble() * maxOpacity, base ) );
  }
  else
  {
    scaleExpression = parseOpacityStops( base, stops, maxOpacity );
  }
  return QgsProperty::fromExpression( scaleExpression );
}

QString QgsMapBoxGlStyleConverter::parseOpacityStops( double base, const QVariantList &stops, int maxOpacity )
{
  QString caseString = QStringLiteral( "CASE WHEN @zoom_level < %1 THEN set_color_part(@symbol_color, 'alpha', %2)" )
                       .arg( stops.value( 0 ).toList().value( 0 ).toString() )
                       .arg( stops.value( 0 ).toList().value( 1 ).toDouble() * maxOpacity );

  for ( int i = 0; i < stops.size() - 1; ++i )
  {
    caseString += QStringLiteral( " WHEN @zoom_level >= %1 AND @zoom_level < %2 "
                                  "THEN set_color_part(@symbol_color, 'alpha', %3)" )
                  .arg( stops.value( i ).toList().value( 0 ).toString(),
                        stops.value( i + 1 ).toList().value( 0 ).toString(),
                        interpolateExpression( stops.value( i ).toList().value( 0 ).toDouble(),
                            stops.value( i + 1 ).toList().value( 0 ).toDouble(),
                            stops.value( i ).toList().value( 1 ).toDouble() * maxOpacity,
                            stops.value( i + 1 ).toList().value( 1 ).toDouble() * maxOpacity, base ) );
  }

  caseString += QStringLiteral( " WHEN @zoom_level >= %1 "
                                "THEN set_color_part(@symbol_color, 'alpha', %2) END" )
                .arg( stops.last().toList().value( 0 ).toString() )
                .arg( stops.last().toList().value( 1 ).toDouble() * maxOpacity );
  return caseString;
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolatePointByZoom( const QVariantMap &json, QgsMapBoxGlStyleConversionContext &context, double multiplier, QPointF *defaultPoint )
{
  const double base = json.value( QStringLiteral( "base" ), QStringLiteral( "1" ) ).toDouble();
  const QVariantList stops = json.value( QStringLiteral( "stops" ) ).toList();
  if ( stops.empty() )
    return QgsProperty();

  QString scaleExpression;
  if ( stops.size() <= 2 )
  {
    scaleExpression = QStringLiteral( "array(%1,%2)" ).arg( interpolateExpression( stops.value( 0 ).toList().value( 0 ).toDouble(),
                      stops.last().toList().value( 0 ).toDouble(),
                      stops.value( 0 ).toList().value( 1 ).toList().value( 0 ).toDouble(),
                      stops.last().toList().value( 1 ).toList().value( 0 ).toDouble(), base, multiplier ),
                      interpolateExpression( stops.value( 0 ).toList().value( 0 ).toDouble(),
                          stops.last().toList().value( 0 ).toDouble(),
                          stops.value( 0 ).toList().value( 1 ).toList().value( 1 ).toDouble(),
                          stops.last().toList().value( 1 ).toList().value( 1 ).toDouble(), base, multiplier )
                                                          );
  }
  else
  {
    scaleExpression = parsePointStops( base, stops, context, multiplier );
  }

  if ( !stops.empty() && defaultPoint )
    *defaultPoint = QPointF( stops.value( 0 ).toList().value( 1 ).toList().value( 0 ).toDouble() * multiplier,
                             stops.value( 0 ).toList().value( 1 ).toList().value( 1 ).toDouble() * multiplier );

  return QgsProperty::fromExpression( scaleExpression );
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolateStringByZoom( const QVariantMap &json, QgsMapBoxGlStyleConversionContext &context,
    const QVariantMap &conversionMap, QString *defaultString )
{
  const QVariantList stops = json.value( QStringLiteral( "stops" ) ).toList();
  if ( stops.empty() )
    return QgsProperty();

  QString scaleExpression = parseStringStops( stops, context, conversionMap, defaultString );

  return QgsProperty::fromExpression( scaleExpression );
}

QString QgsMapBoxGlStyleConverter::parsePointStops( double base, const QVariantList &stops, QgsMapBoxGlStyleConversionContext &context, double multiplier )
{
  QString caseString = QStringLiteral( "CASE " );

  for ( int i = 0; i < stops.length() - 1; ++i )
  {
    // bottom zoom and value
    const QVariant bz = stops.value( i ).toList().value( 0 );
    const QVariant bv = stops.value( i ).toList().value( 1 );
    if ( bz.type() != QVariant::List && bz.type() != QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "Could not convert offset interpolation, skipping." ) );
      return QString();
    }

    // top zoom and value
    const QVariant tz = stops.value( i + 1 ).toList().value( 0 );
    const QVariant tv = stops.value( i + 1 ).toList().value( 1 );
    if ( tz.type() != QVariant::List && tz.type() != QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "Could not convert offset interpolation, skipping." ) );
      return QString();
    }

    caseString += QStringLiteral( "WHEN @zoom_level > %1 AND @zoom_level <= %2 "
                                  "THEN array(%3,%4)" ).arg( bz.toString(),
                                      tz.toString(),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bv.toList().value( 0 ).toDouble(), tv.toList().value( 0 ).toDouble(), base, multiplier ),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bv.toList().value( 1 ).toDouble(), tv.toList().value( 1 ).toDouble(), base, multiplier ) );
  }
  caseString += QStringLiteral( "END" );
  return caseString;
}

QString QgsMapBoxGlStyleConverter::parseStops( double base, const QVariantList &stops, double multiplier, QgsMapBoxGlStyleConversionContext &context )
{
  QString caseString = QStringLiteral( "CASE " );

  for ( int i = 0; i < stops.length() - 1; ++i )
  {
    // bottom zoom and value
    const QVariant bz = stops.value( i ).toList().value( 0 );
    const QVariant bv = stops.value( i ).toList().value( 1 );
    if ( bz.type() == QVariant::List || bz.type() == QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "QGIS does not support expressions in interpolation function, skipping." ) );
      return QString();
    }

    // top zoom and value
    const QVariant tz = stops.value( i + 1 ).toList().value( 0 );
    const QVariant tv = stops.value( i + 1 ).toList().value( 1 );
    if ( tz.type() == QVariant::List || tz.type() == QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "QGIS does not support expressions in interpolation function, skipping." ) );
      return QString();
    }

    caseString += QStringLiteral( "WHEN @zoom_level > %1 AND @zoom_level <= %2 "
                                  "THEN %3 " ).arg( bz.toString(),
                                      tz.toString(),
                                      interpolateExpression( bz.toDouble(), tz.toDouble(), bv.toDouble(), tv.toDouble(), base, multiplier ) );
  }
  caseString += QStringLiteral( "END" );
  return caseString;
}

QString QgsMapBoxGlStyleConverter::parseStringStops( const QVariantList &stops, QgsMapBoxGlStyleConversionContext &context, const QVariantMap &conversionMap, QString *defaultString )
{
  QString caseString = QStringLiteral( "CASE " );

  for ( int i = 0; i < stops.length() - 1; ++i )
  {
    // bottom zoom and value
    const QVariant bz = stops.value( i ).toList().value( 0 );
    const QString bv = stops.value( i ).toList().value( 1 ).toString();
    if ( bz.type() == QVariant::List || bz.type() == QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "QGIS does not support expressions in interpolation function, skipping." ) );
      return QString();
    }

    // top zoom
    const QVariant tz = stops.value( i + 1 ).toList().value( 0 );
    if ( tz.type() == QVariant::List || tz.type() == QVariant::StringList )
    {
      context.pushWarning( QObject::tr( "QGIS does not support expressions in interpolation function, skipping." ) );
      return QString();
    }

    caseString += QStringLiteral( "WHEN @zoom_level > %1 AND @zoom_level <= %2 "
                                  "THEN %3 " ).arg( bz.toString(),
                                      tz.toString(),
                                      QgsExpression::quotedValue( conversionMap.value( bv, bv ) ) );
  }
  caseString += QStringLiteral( "ELSE %1 END" ).arg( QgsExpression::quotedValue( conversionMap.value( stops.constLast().toList().value( 1 ).toString(),
                stops.constLast().toList().value( 1 ) ) ) );
  if ( defaultString )
    *defaultString = stops.constLast().toList().value( 1 ).toString();
  return caseString;
}

QgsProperty QgsMapBoxGlStyleConverter::parseInterpolateListByZoom( const QVariantList &json, PropertyType type, QgsMapBoxGlStyleConversionContext &context, double multiplier, int maxOpacity, QColor *defaultColor, double *defaultNumber )
{
  if ( json.value( 0 ).toString() != QLatin1String( "interpolate" ) )
  {
    context.pushWarning( QObject::tr( "Could not interpret value list" ) );
    return QgsProperty();
  }

  double base = 1;
  const QString technique = json.value( 1 ).toList().value( 0 ).toString();
  if ( technique == QLatin1String( "linear" ) )
    base = 1;
  else if ( technique == QLatin1String( "exponential" ) )
    base = json.value( 1 ).toList(). value( 1 ).toDouble();
  else if ( technique == QLatin1String( "cubic-bezier" ) )
  {
    context.pushWarning( QObject::tr( "QGIS does not support cubic-bezier interpolation, linear used instead." ) );
    base = 1;
  }
  else
  {
    context.pushWarning( QObject::tr( "Skipping not implemented interpolation method %1" ).arg( technique ) );
    return QgsProperty();
  }

  if ( json.value( 2 ).toList().value( 0 ).toString() != QLatin1String( "zoom" ) )
  {
    context.pushWarning( QObject::tr( "Skipping not implemented interpolation input %1" ).arg( json.value( 2 ).toString() ) );
    return QgsProperty();
  }

  //  Convert stops into list of lists
  QVariantList stops;
  for ( int i = 3; i < json.length(); i += 2 )
  {
    stops.push_back( QVariantList() << json.value( i ).toString() << json.value( i + 1 ).toString() );
  }

  QVariantMap props;
  props.insert( QStringLiteral( "stops" ), stops );
  props.insert( QStringLiteral( "base" ), base );
  switch ( type )
  {
    case PropertyType::Color:
      return parseInterpolateColorByZoom( props, context, defaultColor );

    case PropertyType::Numeric:
      return parseInterpolateByZoom( props, context, multiplier, defaultNumber );

    case PropertyType::Opacity:
      return parseInterpolateOpacityByZoom( props, maxOpacity );

    case PropertyType::Point:
      return parseInterpolatePointByZoom( props, context, multiplier );
  }
  return QgsProperty();
}

QColor QgsMapBoxGlStyleConverter::parseColor( const QVariant &color, QgsMapBoxGlStyleConversionContext &context )
{
  if ( color.type() != QVariant::String )
  {
    context.pushWarning( QObject::tr( "Could not parse non-string color %1, skipping" ).arg( color.toString() ) );
    return QColor();
  }

  return QgsSymbolLayerUtils::parseColor( color.toString() );
}

void QgsMapBoxGlStyleConverter::colorAsHslaComponents( const QColor &color, int &hue, int &saturation, int &lightness, int &alpha )
{
  hue = std::max( 0, color.hslHue() );
  saturation = color.hslSaturation() / 255.0 * 100;
  lightness = color.lightness() / 255.0 * 100;
  alpha = color.alpha();
}

QString QgsMapBoxGlStyleConverter::interpolateExpression( double zoomMin, double zoomMax, double valueMin, double valueMax, double base, double multiplier )
{
  // special case!
  if ( qgsDoubleNear( valueMin, valueMax ) )
    return QString::number( valueMin * multiplier );

  QString expression;
  if ( base == 1 )
  {
    expression = QStringLiteral( "scale_linear(@zoom_level,%1,%2,%3,%4)" ).arg( zoomMin )
                 .arg( zoomMax )
                 .arg( valueMin )
                 .arg( valueMax );
  }
  else
  {
    expression = QStringLiteral( "scale_exp(@zoom_level,%1,%2,%3,%4,%5)" ).arg( zoomMin )
                 .arg( zoomMax )
                 .arg( valueMin )
                 .arg( valueMax )
                 .arg( base );
  }

  if ( multiplier != 1 )
    return QStringLiteral( "%1 * %2" ).arg( expression ).arg( multiplier );
  else
    return expression;
}

Qt::PenCapStyle QgsMapBoxGlStyleConverter::parseCapStyle( const QString &style )
{
  if ( style == QLatin1String( "round" ) )
    return Qt::RoundCap;
  else if ( style == QLatin1String( "square" ) )
    return Qt::SquareCap;
  else
    return Qt::FlatCap; // "butt" is default
}

Qt::PenJoinStyle QgsMapBoxGlStyleConverter::parseJoinStyle( const QString &style )
{
  if ( style == QLatin1String( "bevel" ) )
    return Qt::BevelJoin;
  else if ( style == QLatin1String( "round" ) )
    return Qt::RoundJoin;
  else
    return Qt::MiterJoin; // "miter" is default
}

QString QgsMapBoxGlStyleConverter::parseExpression( const QVariantList &expression, QgsMapBoxGlStyleConversionContext &context )
{
  QString op = expression.value( 0 ).toString();
  if ( op == QLatin1String( "all" )
       || op == QLatin1String( "any" )
       || op == QLatin1String( "none" ) )
  {
    QStringList parts;
    for ( int i = 1; i < expression.size(); ++i )
    {
      QString part = parseValue( expression.at( i ), context );
      if ( part.isEmpty() )
      {
        context.pushWarning( QObject::tr( "Skipping unsupported expression" ) );
        return QString();
      }
      parts << part;
    }

    if ( op == QLatin1String( "none" ) )
      return QStringLiteral( "NOT (%1)" ).arg( parts.join( QStringLiteral( ") AND NOT (" ) ) );

    QString operatorString;
    if ( op == QLatin1String( "all" ) )
      operatorString = QStringLiteral( ") AND (" );
    else if ( op == QLatin1String( "any" ) )
      operatorString = QStringLiteral( ") OR (" );

    return QStringLiteral( "(%1)" ).arg( parts.join( operatorString ) );
  }
  else if ( op == '!' )
  {
    // ! inverts next expression's meaning
    QVariantList contraJsonExpr = expression.value( 1 ).toList();
    contraJsonExpr[0] = op + contraJsonExpr[0].toString();
    // ['!', ['has', 'level']] -> ['!has', 'level']
    return parseKey( contraJsonExpr );
  }
  else if ( op == QLatin1String( "==" )
            || op == QLatin1String( "!=" )
            || op == QLatin1String( ">=" )
            || op == '>'
            || op == QLatin1String( "<=" )
            || op == '<' )
  {
    // use IS and NOT IS instead of = and != because they can deal with NULL values
    if ( op == QLatin1String( "==" ) )
      op = QStringLiteral( "IS" );
    else if ( op == QLatin1String( "!=" ) )
      op = QStringLiteral( "IS NOT" );
    return QStringLiteral( "%1 %2 %3" ).arg( parseKey( expression.value( 1 ) ),
           op, parseValue( expression.value( 2 ), context ) );
  }
  else if ( op == QLatin1String( "has" ) )
  {
    return parseKey( expression.value( 1 ) ) + QStringLiteral( " IS NOT NULL" );
  }
  else if ( op == QLatin1String( "!has" ) )
  {
    return parseKey( expression.value( 1 ) ) + QStringLiteral( " IS NULL" );
  }
  else if ( op == QLatin1String( "in" ) || op == QLatin1String( "!in" ) )
  {
    const QString key = parseKey( expression.value( 1 ) );
    QStringList parts;
    for ( int i = 2; i < expression.size(); ++i )
    {
      QString part = parseValue( expression.at( i ), context );
      if ( part.isEmpty() )
      {
        context.pushWarning( QObject::tr( "Skipping unsupported expression" ) );
        return QString();
      }
      parts << part;
    }
    if ( op == QLatin1String( "in" ) )
      return QStringLiteral( "%1 IN (%2)" ).arg( key, parts.join( QStringLiteral( ", " ) ) );
    else
      return QStringLiteral( "(%1 IS NULL OR %1 NOT IN (%2))" ).arg( key, parts.join( QStringLiteral( ", " ) ) );
  }
  else if ( op == QLatin1String( "get" ) )
  {
    return parseKey( expression.value( 1 ) );
  }
  else if ( op == QLatin1String( "match" ) )
  {
    const QString attribute = expression.value( 1 ).toList().value( 1 ).toString();

    if ( expression.size() == 5
         && expression.at( 3 ).type() == QVariant::Bool && expression.at( 3 ).toBool() == true
         && expression.at( 4 ).type() == QVariant::Bool && expression.at( 4 ).toBool() == false )
    {
      // simple case, make a nice simple expression instead of a CASE statement
      if ( expression.at( 2 ).type() == QVariant::List || expression.at( 2 ).type() == QVariant::StringList )
      {
        QStringList parts;
        for ( const QVariant &p : expression.at( 2 ).toList() )
        {
          parts << QgsExpression::quotedValue( p );
        }

        if ( parts.size() > 1 )
          return QStringLiteral( "%1 IN (%2)" ).arg( QgsExpression::quotedColumnRef( attribute ), parts.join( ", " ) );
        else
          return QgsExpression::createFieldEqualityExpression( attribute, expression.at( 2 ).toList().value( 0 ) );
      }
      else if ( expression.at( 2 ).type() == QVariant::String || expression.at( 2 ).type() == QVariant::Int
                || expression.at( 2 ).type() == QVariant::Double )
      {
        return QgsExpression::createFieldEqualityExpression( attribute, expression.at( 2 ) );
      }
      else
      {
        context.pushWarning( QObject::tr( "Skipping non-supported expression" ) );
        return QString();
      }
    }
    else
    {
      QString caseString = QStringLiteral( "CASE " );
      for ( int i = 2; i < expression.size() - 2; i += 2 )
      {
        if ( expression.at( i ).type() == QVariant::List || expression.at( i ).type() == QVariant::StringList )
        {
          QStringList parts;
          for ( const QVariant &p : expression.at( i ).toList() )
          {
            parts << QgsExpression::quotedValue( p );
          }

          if ( parts.size() > 1 )
            caseString += QStringLiteral( "WHEN %1 IN (%2) " ).arg( QgsExpression::quotedColumnRef( attribute ), parts.join( ", " ) );
          else
            caseString += QStringLiteral( "WHEN %1 " ).arg( QgsExpression::createFieldEqualityExpression( attribute, expression.at( i ).toList().value( 0 ) ) );
        }
        else if ( expression.at( i ).type() == QVariant::String || expression.at( i ).type() == QVariant::Int
                  || expression.at( i ).type() == QVariant::Double )
        {
          caseString += QStringLiteral( "WHEN (%1) " ).arg( QgsExpression::createFieldEqualityExpression( attribute, expression.at( i ) ) );
        }

        caseString += QStringLiteral( "THEN %1 " ).arg( QgsExpression::quotedValue( expression.at( i + 1 ) ) );
      }
      caseString += QStringLiteral( "ELSE %1 END" ).arg( QgsExpression::quotedValue( expression.last() ) );
      return caseString;
    }
  }
  else if ( op == QLatin1String( "to-string" ) )
  {
    return QStringLiteral( "to_string(%1)" ).arg( parseExpression( expression.value( 1 ).toList(), context ) );
  }
  else
  {
    context.pushWarning( QObject::tr( "Skipping non-supported expression" ) );
    return QString();
  }
}

QImage QgsMapBoxGlStyleConverter::retrieveSprite( const QString &name, QgsMapBoxGlStyleConversionContext &context )
{
  if ( context.spriteImage().isNull() )
  {
    context.pushWarning( QObject::tr( "Could not retrieve sprite '%1'" ).arg( name ) );
    return QImage();
  }

  const QVariantMap spriteDefinition = context.spriteDefinitions().value( name ).toMap();
  if ( spriteDefinition.size() == 0 )
  {
    context.pushWarning( QObject::tr( "Could not retrieve sprite '%1'" ).arg( name ) );
    return QImage();
  }

  const QImage sprite = context.spriteImage().copy( spriteDefinition.value( QStringLiteral( "x" ) ).toInt(),
                        spriteDefinition.value( QStringLiteral( "y" ) ).toInt(),
                        spriteDefinition.value( QStringLiteral( "width" ) ).toInt(),
                        spriteDefinition.value( QStringLiteral( "height" ) ).toInt() );
  if ( sprite.isNull() )
  {
    context.pushWarning( QObject::tr( "Could not retrieve sprite '%1'" ).arg( name ) );
    return QImage();
  }

  return sprite;
}

QString QgsMapBoxGlStyleConverter::retrieveSpriteAsBase64( const QString &name, QgsMapBoxGlStyleConversionContext &context, QSize &size )
{
  const QImage sprite = retrieveSprite( name, context );
  if ( !sprite.isNull() )
  {
    size = sprite.size();
    QByteArray blob;
    QBuffer buffer( &blob );
    buffer.open( QIODevice::WriteOnly );
    sprite.save( &buffer, "PNG" );
    buffer.close();
    QByteArray encoded = blob.toBase64();

    QString path( encoded );
    path.prepend( QLatin1String( "base64:" ) );
    return path;
  }
  return QString();
}

QString QgsMapBoxGlStyleConverter::parseValue( const QVariant &value, QgsMapBoxGlStyleConversionContext &context )
{
  switch ( value.type() )
  {
    case QVariant::List:
    case QVariant::StringList:
      return parseExpression( value.toList(), context );

    case QVariant::String:
      return QgsExpression::quotedValue( value.toString() );

    case QVariant::Int:
    case QVariant::Double:
      return value.toString();

    default:
      context.pushWarning( QObject::tr( "Skipping unsupported expression part" ) );
      break;
  }
  return QString();
}

QString QgsMapBoxGlStyleConverter::parseKey( const QVariant &value )
{
  if ( value.toString() == QLatin1String( "$type" ) )
    return QStringLiteral( "_geom_type" );
  else if ( value.type() == QVariant::List || value.type() == QVariant::StringList )
  {
    if ( value.toList().size() > 1 )
      return value.toList().at( 1 ).toString();
    else
      return value.toList().value( 0 ).toString();
  }
  return QgsExpression::quotedColumnRef( value.toString() );
}

QgsVectorTileRenderer *QgsMapBoxGlStyleConverter::renderer() const
{
  return mRenderer ? mRenderer->clone() : nullptr;
}

QgsVectorTileLabeling *QgsMapBoxGlStyleConverter::labeling() const
{
  return mLabeling ? mLabeling->clone() : nullptr;
}

//
// QgsMapBoxGlStyleConversionContext
//
void QgsMapBoxGlStyleConversionContext::pushWarning( const QString &warning )
{
  QgsDebugMsg( warning );
  mWarnings << warning;
}

QgsUnitTypes::RenderUnit QgsMapBoxGlStyleConversionContext::targetUnit() const
{
  return mTargetUnit;
}

void QgsMapBoxGlStyleConversionContext::setTargetUnit( QgsUnitTypes::RenderUnit targetUnit )
{
  mTargetUnit = targetUnit;
}

double QgsMapBoxGlStyleConversionContext::pixelSizeConversionFactor() const
{
  return mSizeConversionFactor;
}

void QgsMapBoxGlStyleConversionContext::setPixelSizeConversionFactor( double sizeConversionFactor )
{
  mSizeConversionFactor = sizeConversionFactor;
}

QImage QgsMapBoxGlStyleConversionContext::spriteImage() const
{
  return mSpriteImage;
}

QVariantMap QgsMapBoxGlStyleConversionContext::spriteDefinitions() const
{
  return mSpriteDefinitions;
}

void QgsMapBoxGlStyleConversionContext::setSprites( const QImage &image, const QVariantMap &definitions )
{
  mSpriteImage = image;
  mSpriteDefinitions = definitions;
}

void QgsMapBoxGlStyleConversionContext::setSprites( const QImage &image, const QString &definitions )
{
  setSprites( image, QgsJsonUtils::parseJson( definitions ).toMap() );
}

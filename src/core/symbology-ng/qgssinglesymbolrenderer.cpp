/***************************************************************************
    qgssinglesymbolrenderer.cpp
    ---------------------
    begin                : November 2009
    copyright            : (C) 2009 by Martin Dobias
    email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgssinglesymbolrenderer.h"

#include "qgssymbol.h"
#include "qgssymbollayerutils.h"

#include "qgslogger.h"
#include "qgsfeature.h"
#include "qgsvectorlayer.h"
#include "qgssymbollayer.h"
#include "qgsogcutils.h"
#include "qgspointdisplacementrenderer.h"
#include "qgsinvertedpolygonrenderer.h"
#include "qgspainteffect.h"
#include "qgspainteffectregistry.h"
#include "qgsscaleexpression.h"
#include "qgsdatadefined.h"

#include <QDomDocument>
#include <QDomElement>

QgsSingleSymbolRenderer::QgsSingleSymbolRenderer( QgsSymbol* symbol )
    : QgsFeatureRenderer( QStringLiteral( "singleSymbol" ) )
    , mSymbol( symbol )
{
  Q_ASSERT( symbol );
}

QgsSymbol* QgsSingleSymbolRenderer::symbolForFeature( QgsFeature&, QgsRenderContext & )
{
  return mSymbol.data();
}

QgsSymbol* QgsSingleSymbolRenderer::originalSymbolForFeature( QgsFeature& feature, QgsRenderContext &context )
{
  Q_UNUSED( context );
  Q_UNUSED( feature );
  return mSymbol.data();
}

void QgsSingleSymbolRenderer::startRender( QgsRenderContext& context, const QgsFields& fields )
{
  if ( !mSymbol.data() )
    return;

  mSymbol->startRender( context, fields );
}

void QgsSingleSymbolRenderer::stopRender( QgsRenderContext& context )
{
  if ( !mSymbol.data() )
    return;

  mSymbol->stopRender( context );
}

QSet<QString> QgsSingleSymbolRenderer::usedAttributes() const
{
  QSet<QString> attributes;
  if ( mSymbol.data() )
    attributes.unite( mSymbol->usedAttributes() );
  return attributes;
}

QgsSymbol* QgsSingleSymbolRenderer::symbol() const
{
  return mSymbol.data();
}

void QgsSingleSymbolRenderer::setSymbol( QgsSymbol* s )
{
  Q_ASSERT( s );
  mSymbol.reset( s );
}

QString QgsSingleSymbolRenderer::dump() const
{
  return mSymbol.data() ? QStringLiteral( "SINGLE: %1" ).arg( mSymbol->dump() ) : QLatin1String( "" );
}

QgsSingleSymbolRenderer* QgsSingleSymbolRenderer::clone() const
{
  QgsSingleSymbolRenderer* r = new QgsSingleSymbolRenderer( mSymbol->clone() );
  r->setUsingSymbolLevels( usingSymbolLevels() );
  copyRendererData( r );
  return r;
}

void QgsSingleSymbolRenderer::toSld( QDomDocument& doc, QDomElement &element, const QgsStringMap &props ) const
{
  QgsStringMap newProps = props;

  QDomElement ruleElem = doc.createElement( QStringLiteral( "se:Rule" ) );
  element.appendChild( ruleElem );

  QDomElement nameElem = doc.createElement( QStringLiteral( "se:Name" ) );
  nameElem.appendChild( doc.createTextNode( QStringLiteral( "Single symbol" ) ) );
  ruleElem.appendChild( nameElem );

  QgsSymbolLayerUtils::applyScaleDependency( doc, ruleElem, newProps );

  if ( mSymbol.data() ) mSymbol->toSld( doc, ruleElem, newProps );
}

QgsSymbolList QgsSingleSymbolRenderer::symbols( QgsRenderContext &context )
{
  Q_UNUSED( context );
  QgsSymbolList lst;
  lst.append( mSymbol.data() );
  return lst;
}


QgsFeatureRenderer* QgsSingleSymbolRenderer::create( QDomElement& element )
{
  QDomElement symbolsElem = element.firstChildElement( QStringLiteral( "symbols" ) );
  if ( symbolsElem.isNull() )
    return nullptr;

  QgsSymbolMap symbolMap = QgsSymbolLayerUtils::loadSymbols( symbolsElem );

  if ( !symbolMap.contains( QStringLiteral( "0" ) ) )
    return nullptr;

  QgsSingleSymbolRenderer* r = new QgsSingleSymbolRenderer( symbolMap.take( QStringLiteral( "0" ) ) );

  // delete symbols if there are any more
  QgsSymbolLayerUtils::clearSymbolMap( symbolMap );

  QDomElement rotationElem = element.firstChildElement( QStringLiteral( "rotation" ) );
  if ( !rotationElem.isNull() && !rotationElem.attribute( QStringLiteral( "field" ) ).isEmpty() )
  {
    convertSymbolRotation( r->mSymbol.data(), rotationElem.attribute( QStringLiteral( "field" ) ) );
  }

  QDomElement sizeScaleElem = element.firstChildElement( QStringLiteral( "sizescale" ) );
  if ( !sizeScaleElem.isNull() && !sizeScaleElem.attribute( QStringLiteral( "field" ) ).isEmpty() )
  {
    convertSymbolSizeScale( r->mSymbol.data(),
                            QgsSymbolLayerUtils::decodeScaleMethod( sizeScaleElem.attribute( QStringLiteral( "scalemethod" ) ) ),
                            sizeScaleElem.attribute( QStringLiteral( "field" ) ) );
  }

  // TODO: symbol levels
  return r;
}

QgsFeatureRenderer* QgsSingleSymbolRenderer::createFromSld( QDomElement& element, QgsWkbTypes::GeometryType geomType )
{
  // XXX this renderer can handle only one Rule!

  // get the first Rule element
  QDomElement ruleElem = element.firstChildElement( QStringLiteral( "Rule" ) );
  if ( ruleElem.isNull() )
  {
    QgsDebugMsg( "no Rule elements found!" );
    return nullptr;
  }

  QString label, description;
  QgsSymbolLayerList layers;

  // retrieve the Rule element child nodes
  QDomElement childElem = ruleElem.firstChildElement();
  while ( !childElem.isNull() )
  {
    if ( childElem.localName() == QLatin1String( "Name" ) )
    {
      // <se:Name> tag contains the rule identifier,
      // so prefer title tag for the label property value
      if ( label.isEmpty() )
        label = childElem.firstChild().nodeValue();
    }
    else if ( childElem.localName() == QLatin1String( "Description" ) )
    {
      // <se:Description> can contains a title and an abstract
      QDomElement titleElem = childElem.firstChildElement( QStringLiteral( "Title" ) );
      if ( !titleElem.isNull() )
      {
        label = titleElem.firstChild().nodeValue();
      }

      QDomElement abstractElem = childElem.firstChildElement( QStringLiteral( "Abstract" ) );
      if ( !abstractElem.isNull() )
      {
        description = abstractElem.firstChild().nodeValue();
      }
    }
    else if ( childElem.localName() == QLatin1String( "Abstract" ) )
    {
      // <sld:Abstract> (v1.0)
      description = childElem.firstChild().nodeValue();
    }
    else if ( childElem.localName() == QLatin1String( "Title" ) )
    {
      // <sld:Title> (v1.0)
      label = childElem.firstChild().nodeValue();
    }
    else if ( childElem.localName().endsWith( QLatin1String( "Symbolizer" ) ) )
    {
      // create symbol layers for this symbolizer
      QgsSymbolLayerUtils::createSymbolLayerListFromSld( childElem, geomType, layers );
    }

    childElem = childElem.nextSiblingElement();
  }

  if ( layers.isEmpty() )
    return nullptr;

  // now create the symbol
  QgsSymbol *symbol;
  switch ( geomType )
  {
    case QgsWkbTypes::LineGeometry:
      symbol = new QgsLineSymbol( layers );
      break;

    case QgsWkbTypes::PolygonGeometry:
      symbol = new QgsFillSymbol( layers );
      break;

    case QgsWkbTypes::PointGeometry:
      symbol = new QgsMarkerSymbol( layers );
      break;

    default:
      QgsDebugMsg( QString( "invalid geometry type: found %1" ).arg( geomType ) );
      return nullptr;
  }

  // and finally return the new renderer
  return new QgsSingleSymbolRenderer( symbol );
}

QDomElement QgsSingleSymbolRenderer::save( QDomDocument& doc )
{
  QDomElement rendererElem = doc.createElement( RENDERER_TAG_NAME );
  rendererElem.setAttribute( QStringLiteral( "type" ), QStringLiteral( "singleSymbol" ) );
  rendererElem.setAttribute( QStringLiteral( "symbollevels" ), ( mUsingSymbolLevels ? "1" : "0" ) );
  rendererElem.setAttribute( QStringLiteral( "forceraster" ), ( mForceRaster ? "1" : "0" ) );

  QgsSymbolMap symbols;
  symbols[QStringLiteral( "0" )] = mSymbol.data();
  QDomElement symbolsElem = QgsSymbolLayerUtils::saveSymbols( symbols, QStringLiteral( "symbols" ), doc );
  rendererElem.appendChild( symbolsElem );

  QDomElement rotationElem = doc.createElement( QStringLiteral( "rotation" ) );
  rendererElem.appendChild( rotationElem );

  QDomElement sizeScaleElem = doc.createElement( QStringLiteral( "sizescale" ) );
  rendererElem.appendChild( sizeScaleElem );

  if ( mPaintEffect && !QgsPaintEffectRegistry::isDefaultStack( mPaintEffect ) )
    mPaintEffect->saveProperties( doc, rendererElem );

  if ( !mOrderBy.isEmpty() )
  {
    QDomElement orderBy = doc.createElement( QStringLiteral( "orderby" ) );
    mOrderBy.save( orderBy );
    rendererElem.appendChild( orderBy );
  }
  rendererElem.setAttribute( QStringLiteral( "enableorderby" ), ( mOrderByEnabled ? "1" : "0" ) );

  return rendererElem;
}

QgsLegendSymbologyList QgsSingleSymbolRenderer::legendSymbologyItems( QSize iconSize )
{
  QgsLegendSymbologyList lst;
  if ( mSymbol.data() )
  {
    QPixmap pix = QgsSymbolLayerUtils::symbolPreviewPixmap( mSymbol.data(), iconSize );
    lst << qMakePair( QString(), pix );
  }
  return lst;
}

QgsLegendSymbolList QgsSingleSymbolRenderer::legendSymbolItems( double scaleDenominator, const QString& rule )
{
  Q_UNUSED( scaleDenominator );
  Q_UNUSED( rule );
  QgsLegendSymbolList lst;
  lst << qMakePair( QString(), mSymbol.data() );
  return lst;
}

QgsLegendSymbolListV2 QgsSingleSymbolRenderer::legendSymbolItemsV2() const
{
  QgsLegendSymbolListV2 lst;
  if ( mSymbol->type() == QgsSymbol::Marker )
  {
    const QgsMarkerSymbol * symbol = static_cast<const QgsMarkerSymbol *>( mSymbol.data() );
    QgsDataDefined sizeDD = symbol->dataDefinedSize();
    if ( sizeDD.isActive() && sizeDD.useExpression() )
    {
      QgsScaleExpression scaleExp( sizeDD.expressionString() );
      if ( scaleExp.type() != QgsScaleExpression::Unknown )
      {
        QgsLegendSymbolItem title( nullptr, scaleExp.baseExpression(), QString() );
        lst << title;
        Q_FOREACH ( double v, QgsSymbolLayerUtils::prettyBreaks( scaleExp.minValue(), scaleExp.maxValue(), 4 ) )
        {
          QgsLegendSymbolItem si( mSymbol.data(), QString::number( v ), QString() );
          QgsMarkerSymbol * s = static_cast<QgsMarkerSymbol *>( si.symbol() );
          s->setDataDefinedSize( 0 );
          s->setSize( scaleExp.size( v ) );
          lst << si;
        }
        return lst;
      }
    }
  }

  lst << QgsLegendSymbolItem( mSymbol.data(), QString(), QString() );
  return lst;
}

QSet< QString > QgsSingleSymbolRenderer::legendKeysForFeature( QgsFeature& feature, QgsRenderContext& context )
{
  Q_UNUSED( feature );
  Q_UNUSED( context );
  return QSet< QString >() << QString();
}

void QgsSingleSymbolRenderer::setLegendSymbolItem( const QString& key, QgsSymbol* symbol )
{
  Q_UNUSED( key );
  setSymbol( symbol );
}

QgsSingleSymbolRenderer* QgsSingleSymbolRenderer::convertFromRenderer( const QgsFeatureRenderer *renderer )
{
  QgsSingleSymbolRenderer* r = nullptr;
  if ( renderer->type() == QLatin1String( "singleSymbol" ) )
  {
    r = dynamic_cast<QgsSingleSymbolRenderer*>( renderer->clone() );
  }
  else if ( renderer->type() == QLatin1String( "pointDisplacement" ) || renderer->type() == QLatin1String( "pointCluster" ) )
  {
    const QgsPointDistanceRenderer* pointDistanceRenderer = dynamic_cast<const QgsPointDistanceRenderer*>( renderer );
    if ( pointDistanceRenderer )
      r = convertFromRenderer( pointDistanceRenderer->embeddedRenderer() );
  }
  else if ( renderer->type() == QLatin1String( "invertedPolygonRenderer" ) )
  {
    const QgsInvertedPolygonRenderer* invertedPolygonRenderer = dynamic_cast<const QgsInvertedPolygonRenderer*>( renderer );
    if ( invertedPolygonRenderer )
      r = convertFromRenderer( invertedPolygonRenderer->embeddedRenderer() );
  }

  if ( !r )
  {
    QgsRenderContext context;
    QgsSymbolList symbols = const_cast<QgsFeatureRenderer *>( renderer )->symbols( context );
    if ( !symbols.isEmpty() )
    {
      r = new QgsSingleSymbolRenderer( symbols.at( 0 )->clone() );
    }
  }

  if ( r )
  {
    r->setOrderBy( renderer->orderBy() );
    r->setOrderByEnabled( renderer->orderByEnabled() );
  }

  return r;
}

/*
 * Copyright (c) 2010-2020 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "drawpool.h"
#include "declarations.h"
#include <framework/core/declarations.h>
#include <framework/graphics/framebuffermanager.h>
#include <framework/graphics/graphics.h>
#include "painter.h"
#include <client/config.h>

DrawPool g_drawPool;

void DrawPool::init() {}
void DrawPool::terminate()
{
	m_currentFrameBuffer = nullptr;
}

void DrawPool::add(const std::shared_ptr<CoordsBuffer>& coordsBuffer, const TexturePtr& texture, const FrameBuffer::DrawMethod& method, const Painter::DrawMode drawMode)
{
	if(!m_currentFrameBuffer || !m_currentFrameBuffer->isDrawable()) return;

	m_currentFrameBuffer->updateHash(texture, method);

	auto currentState = g_painter->getCurrentState();
	currentState.texture = texture;

	if(method.type == FrameBuffer::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT) {
		const auto itFind = std::find_if(m_currentFrameBuffer->m_actionObjects.begin(), m_currentFrameBuffer->m_actionObjects.end(),
																		 [currentState](const std::shared_ptr<FrameBuffer::ScheduledAction>& action) { return action->state.isEqual(currentState); });

		if(itFind != m_currentFrameBuffer->m_actionObjects.end()) {
			(*itFind)->drawMethods.push_back(method);
			return;
		}
	} else

		if(!m_currentFrameBuffer->m_actionObjects.empty() &&
			 !(method.type == FrameBuffer::DrawMethodType::DRAW_TEXTURE_COORDS || method.type == FrameBuffer::DrawMethodType::DRAW_FILL_COORDS)) {
			auto& prevDrawObject = m_currentFrameBuffer->m_actionObjects.back();

			if(prevDrawObject->state.isEqual(currentState)) {
				prevDrawObject->drawMode = Painter::DrawMode::Triangles;

				// Search for identical objects in the same position
				bool push = false;
				if(!method.dest.isNull()) {
					const auto itFind = std::find_if(prevDrawObject->drawMethods.begin(), prevDrawObject->drawMethods.end(),
																					 [&](const FrameBuffer::DrawMethod& prevMethod) { return prevMethod.dest == method.dest; });
					push = itFind != prevDrawObject->drawMethods.end();
				}

				if(!push) {
					prevDrawObject->drawMethods.push_back(method);
					auto& list = m_currentFrameBuffer->m_coordsActionObjects[method.dest.hash()];
					list.push_back(prevDrawObject);
				}

				return;
			}
		}

	const auto& actionObject = std::make_shared<FrameBuffer::ScheduledAction>(FrameBuffer::ScheduledAction{ 0, currentState, coordsBuffer, drawMode, {method} });

	// Look for identical or opaque textures that are greater than or equal to the size of the previous texture and remove.
	if(method.type == FrameBuffer::DrawMethodType::DRAW_TEXTURED_RECT && currentState.compositionMode != Painter::CompositionMode_Multiply &&
		 !method.dest.isNull() && texture && currentState.opacity >= 1.f && currentState.color.aF() >= 1.f) {
		auto& list = m_currentFrameBuffer->m_coordsActionObjects[method.dest.hash()];
		for(auto& prevAction : list) {
			if(prevAction->state.texture == texture || texture->isOpaque() && prevAction->state.texture->getRealSize() <= SPRITE_SIZE) {
				for(auto itm = prevAction->drawMethods.begin(); itm != prevAction->drawMethods.end(); ++itm) {
					auto& prevMethod = *itm;
					if(prevMethod.dest == method.dest) {
						prevAction->drawMethods.erase(itm);
						break;
					}
				}
			}
		}

		list.push_back(actionObject);
	}

	m_currentFrameBuffer->m_actionObjects.push_back(actionObject);
}

bool DrawPool::canFill(const FrameBufferPtr& frameBuffer)
{
	m_currentFrameBuffer = frameBuffer;
	if(!frameBuffer || !canUpdate())
		return false;

	frameBuffer->resetCurrentStatus();
	return true;
}

void DrawPool::draw(const FrameBufferPtr& frameBuffer, const Rect& dest, const Rect& src)
{
	if(!frameBuffer->isDrawable()) return;

	g_painter->saveAndResetState();
	if(frameBuffer->hasModification()) {
		frameBuffer->updateStatus();
		frameBuffer->bind();

		for(auto& obj : frameBuffer->m_actionObjects)
			drawObject(*obj);

		frameBuffer->release();

		frameBuffer->m_lastCoordsCache = frameBuffer->m_currentCoordsCache;
		frameBuffer->m_currentCoordsCache.clear();
	}

	frameBuffer->m_coordsActionObjects.clear();
	frameBuffer->m_actionObjects.clear();

	frameBuffer->draw(dest, src);

	g_painter->restoreSavedState();
}

void DrawPool::drawObject(const FrameBuffer::ScheduledAction& obj)
{
	if(obj.action) {
		obj.action();
		return;
	}

	if(obj.drawMethods.empty()) return;

	g_painter->executeState(obj.state);

	if(obj.coordsBuffer != nullptr) {
		g_painter->drawCoords(*obj.coordsBuffer, obj.drawMode);
		return;
	}

	const auto& addCoord = [obj](CoordsBuffer& coord) {
		for(const auto& method : obj.drawMethods) {
			if(method.type == FrameBuffer::DrawMethodType::DRAW_BOUNDING_RECT) {
				coord.addBoudingRect(method.rects.first, method.intValue);
			} else if(method.type == FrameBuffer::DrawMethodType::DRAW_FILLED_RECT) {
				coord.addRect(method.rects.first);
			} else if(method.type == FrameBuffer::DrawMethodType::DRAW_FILLED_TRIANGLE) {
				coord.addTriangle(std::get<0>(method.points), std::get<1>(method.points), std::get<2>(method.points));
			} else if(method.type == FrameBuffer::DrawMethodType::DRAW_TEXTURED_RECT || method.type == FrameBuffer::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT) {
				if(obj.drawMode == Painter::DrawMode::Triangles)
					coord.addRect(method.rects.first, method.rects.second);
				else
					coord.addQuad(method.rects.first, method.rects.second);
			} else if(method.type == FrameBuffer::DrawMethodType::DRAW_UPSIDEDOWN_TEXTURED_RECT) {
				if(obj.drawMode == Painter::DrawMode::Triangles)
					coord.addUpsideDownRect(method.rects.first, method.rects.second);
				else
					coord.addUpsideDownQuad(method.rects.first, method.rects.second);
			}
		}

		return &coord;
	};

	if(obj.drawMethods[0].type == FrameBuffer::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT) {
		size_t hash = m_currentFrameBuffer->HASH_INT(obj.state.color.rgba());
		boost::hash_combine(hash, m_currentFrameBuffer->HASH_INT(obj.state.texture->getId()));

		auto coordCached = m_currentFrameBuffer->m_lastCoordsCache[hash];
		if(!coordCached) {
			coordCached = std::make_shared<CoordsBuffer>();
			coordCached->enableHardwareCaching();			
		}
		
		if(addCoord(m_coordsBuffer)->getVertexHash() != coordCached->getVertexHash()) {
			coordCached->clear();
			addCoord(*coordCached);
		}

		g_painter->drawCoords(*coordCached, obj.drawMode);

		m_currentFrameBuffer->m_currentCoordsCache[hash] = coordCached;
		return;
	}

	if(addCoord(m_coordsBuffer)->canCache()) {
		size_t hash = m_coordsBuffer.getVertexHash();
		auto coordCached = m_currentFrameBuffer->m_lastCoordsCache[hash];
		if(!coordCached) {
			coordCached = std::make_shared<CoordsBuffer>();
			coordCached->enableHardwareCaching(HardwareBuffer::StaticDraw);
			addCoord(*coordCached);
		}

		m_currentFrameBuffer->m_currentCoordsCache[hash] = coordCached;
		g_painter->drawCoords(*coordCached, obj.drawMode);
	} else {
		g_painter->drawCoords(m_coordsBuffer, obj.drawMode);
	}

	m_coordsBuffer.clear();
}

void DrawPool::addFillCoords(CoordsBuffer& coordsBuffer)
{
	FrameBuffer::DrawMethod method;
	method.type = FrameBuffer::DrawMethodType::DRAW_FILL_COORDS;
	method.intValue = coordsBuffer.getVertexHash();

	add(std::shared_ptr<CoordsBuffer>(&coordsBuffer, [](CoordsBuffer*) {}), nullptr, method);
}

void DrawPool::addTextureCoords(CoordsBuffer& coordsBuffer, const TexturePtr& texture, const Painter::DrawMode drawMode)
{
	if(texture && texture->isEmpty())
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_TEXTURE_COORDS };
	method.intValue = coordsBuffer.getVertexHash();

	m_currentFrameBuffer->updateHash(texture, method);

	auto currentState = g_painter->getCurrentState();
	currentState.texture = texture;

	m_currentFrameBuffer->m_actionObjects.push_back(std::make_shared<FrameBuffer::ScheduledAction>(
		FrameBuffer::ScheduledAction{ 0, currentState, std::shared_ptr<CoordsBuffer>(&coordsBuffer, [](CoordsBuffer*) {}), drawMode, {method} }));
}

void DrawPool::addTexturedRect(const Rect& dest, const TexturePtr& texture)
{
	addTexturedRect(dest, texture, Rect(Point(), texture->getSize()));
}

void DrawPool::addTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src, const Point& originalDest)
{
	if(dest.isEmpty() || src.isEmpty() || texture->isEmpty())
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_TEXTURED_RECT };
	method.rects = std::make_pair(dest, src);
	method.dest = originalDest;

	add(nullptr, texture, method, Painter::DrawMode::TriangleStrip);
}

void DrawPool::addUpsideDownTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src)
{
	if(dest.isEmpty() || src.isEmpty() || texture->isEmpty())
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_UPSIDEDOWN_TEXTURED_RECT };
	method.rects = std::make_pair(dest, src);

	add(nullptr, texture, method, Painter::DrawMode::TriangleStrip);
}

void DrawPool::addRepeatedTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src)
{
	if(dest.isEmpty() || src.isEmpty() || texture->isEmpty())
		return;

	size_t hash = m_currentFrameBuffer->HASH_INT(g_painter->getColor().rgba());
	boost::hash_combine(hash, m_currentFrameBuffer->HASH_INT(texture->getId()));

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT };
	method.rects = std::make_pair(dest, src);

	add(std::shared_ptr<CoordsBuffer>(nullptr, [](CoordsBuffer*) {}), texture, method);
}

void DrawPool::addFilledRect(const Rect& dest)
{
	if(dest.isEmpty())
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_FILLED_RECT };
	method.rects = std::make_pair(dest, Rect());

	add(nullptr, nullptr, method);
}

void DrawPool::addFilledTriangle(const Point& a, const Point& b, const Point& c)
{
	if(a == b || a == c || b == c)
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_FILLED_TRIANGLE };
	method.points = std::make_tuple(a, b, c);

	add(nullptr, nullptr, method);
}

void DrawPool::addBoundingRect(const Rect& dest, int innerLineWidth)
{
	if(dest.isEmpty() || innerLineWidth == 0)
		return;

	FrameBuffer::DrawMethod method{ FrameBuffer::DrawMethodType::DRAW_BOUNDING_RECT };
	method.rects = std::make_pair(dest, Rect());
	method.intValue = innerLineWidth;

	add(nullptr, nullptr, method);
}

void DrawPool::addAction(std::function<void()> action)
{
	if(!m_currentFrameBuffer) return;

	m_currentFrameBuffer->m_actionObjects
		.push_back(std::make_shared<FrameBuffer::ScheduledAction>(FrameBuffer::ScheduledAction{ 0, {}, nullptr, Painter::DrawMode::None, {}, action }));
}

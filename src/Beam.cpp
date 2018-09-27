// Copyright © 2008-2016 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "libs.h"
#include "Pi.h"
#include "Beam.h"
#include "Frame.h"
#include "galaxy/StarSystem.h"
#include "Space.h"
#include "GameSaveError.h"
#include "collider/collider.h"
#include "CargoBody.h"
#include "Planet.h"
#include "Sfx.h"
#include "Ship.h"
#include "Pi.h"
#include "Player.h"
#include "Game.h"
#include "LuaEvent.h"
#include "LuaUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/Renderer.h"
#include "graphics/VertexArray.h"
#include "graphics/TextureBuilder.h"
#include "JsonUtils.h"

namespace
{
	static float lifetime = 0.1f;
}

std::unique_ptr<Graphics::VertexArray> Beam::s_sideVerts;
std::unique_ptr<Graphics::VertexArray> Beam::s_glowVerts;
std::unique_ptr<Graphics::Material> Beam::s_sideMat;
std::unique_ptr<Graphics::Material> Beam::s_glowMat;
Graphics::RenderState *Beam::s_renderState = nullptr;

void Beam::BuildModel()
{
	//set up materials
	Graphics::MaterialDescriptor desc;
	desc.textures = 1;
	s_sideMat.reset(Pi::renderer->CreateMaterial(desc));
	s_glowMat.reset(Pi::renderer->CreateMaterial(desc));
	s_sideMat->texture0 = Graphics::TextureBuilder::Billboard("textures/beam_l.dds").GetOrCreateTexture(Pi::renderer, "billboard");
	s_glowMat->texture0 = Graphics::TextureBuilder::Billboard("textures/projectile_w.dds").GetOrCreateTexture(Pi::renderer, "billboard");

	//zero at projectile position
	//+x down
	//+y right
	//+z forwards (or projectile direction)
	const float w = 0.5f;

	vector3f one(0.f, -w, 0.f); //top left
	vector3f two(0.f,  w, 0.f); //top right
	vector3f three(0.f,  w, -1.f); //bottom right
	vector3f four(0.f, -w, -1.f); //bottom left

	//uv coords
	const vector2f topLeft(0.f, 1.f);
	const vector2f topRight(1.f, 1.f);
	const vector2f botLeft(0.f, 0.f);
	const vector2f botRight(1.f, 0.f);

	s_sideVerts.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 24));
	s_glowVerts.reset(new Graphics::VertexArray(Graphics::ATTRIB_POSITION | Graphics::ATTRIB_UV0, 240));

	//add four intersecting planes to create a volumetric effect
	for (int i=0; i < 4; i++) {
		s_sideVerts->Add(one, topLeft);
		s_sideVerts->Add(two, topRight);
		s_sideVerts->Add(three, botRight);

		s_sideVerts->Add(three, botRight);
		s_sideVerts->Add(four, botLeft);
		s_sideVerts->Add(one, topLeft);

		one.ArbRotate(vector3f(0.f, 0.f, 1.f), DEG2RAD(45.f));
		two.ArbRotate(vector3f(0.f, 0.f, 1.f), DEG2RAD(45.f));
		three.ArbRotate(vector3f(0.f, 0.f, 1.f), DEG2RAD(45.f));
		four.ArbRotate(vector3f(0.f, 0.f, 1.f), DEG2RAD(45.f));
	}

	//create quads for viewing on end
	static const float gw = 0.5f;
	float gz = -0.1f;

	for (int i=0; i < 40; i++) {
		s_glowVerts->Add(vector3f(-gw, -gw, gz), topLeft);
		s_glowVerts->Add(vector3f(-gw, gw, gz), topRight);
		s_glowVerts->Add(vector3f(gw, gw, gz), botRight);

		s_glowVerts->Add(vector3f(gw, gw, gz), botRight);
		s_glowVerts->Add(vector3f(gw, -gw, gz), botLeft);
		s_glowVerts->Add(vector3f(-gw, -gw, gz), topLeft);

		gz -= 0.02f; // as they move back
	}

	Graphics::RenderStateDesc rsd;
	rsd.blendMode = Graphics::BLEND_ALPHA_ONE;
	rsd.depthWrite = false;
	rsd.cullMode = Graphics::CULL_NONE;
	s_renderState = Pi::renderer->CreateRenderState(rsd);
}

void Beam::FreeModel()
{
	s_sideMat.reset();
	s_glowMat.reset();
	s_sideVerts.reset();
	s_glowVerts.reset();
}

Beam::Beam(): Body()
{
	if (!s_sideMat) BuildModel();
	SetOrient(matrix3x3d::Identity());
	m_baseDam = 0;
	m_length = 0;
	m_mining = false;
	m_parent = 0;
	m_flags |= FLAG_DRAW_LAST;
	m_age = 0;
	m_active = true;
}

Beam::~Beam()
{
}

void Beam::SaveToJson(Json::Value &jsonObj, Space *space)
{
	Body::SaveToJson(jsonObj, space);

	Json::Value projectileObj(Json::objectValue); // Create JSON object to contain projectile data.

	VectorToJson(projectileObj, m_dir, "dir");
	projectileObj["base_dam"] = FloatToStr(m_baseDam);
	projectileObj["length"] = FloatToStr(m_length);
	projectileObj["mining"] = m_mining;
	ColorToJson(projectileObj, m_color, "color");
	projectileObj["index_for_body"] = space->GetIndexForBody(m_parent);

	jsonObj["projectile"] = projectileObj; // Add projectile object to supplied object.
}

void Beam::LoadFromJson(const Json::Value &jsonObj, Space *space)
{
	Body::LoadFromJson(jsonObj, space);

	if (!jsonObj.isMember("projectile")) throw SavedGameCorruptException();
	Json::Value projectileObj = jsonObj["projectile"];

	if (!projectileObj.isMember("base_dam")) throw SavedGameCorruptException();
	if (!projectileObj.isMember("length")) throw SavedGameCorruptException();
	if (!projectileObj.isMember("mining")) throw SavedGameCorruptException();
	if (!projectileObj.isMember("color")) throw SavedGameCorruptException();
	if (!projectileObj.isMember("index_for_body")) throw SavedGameCorruptException();

	JsonToVector(&m_dir, projectileObj, "dir");
	m_baseDam = StrToFloat(projectileObj["base_dam"].asString());
	m_length = StrToFloat(projectileObj["length"].asString());
	m_mining = projectileObj["mining"].asBool();
	JsonToColor(&m_color, projectileObj, "color");
	m_parentIndex = projectileObj["index_for_body"].asInt();
}

void Beam::PostLoadFixup(Space *space)
{
	Body::PostLoadFixup(space);
	m_parent = space->GetBodyByIndex(m_parentIndex);
}

void Beam::UpdateInterpTransform(double alpha)
{
	m_interpOrient = GetOrient();
	const vector3d oldPos = GetPosition() - (m_baseVel * Pi::game->GetTimeStep());
	m_interpPos = alpha*GetPosition() + (1.0-alpha)*oldPos;
}

void Beam::NotifyRemoved(const Body* const removedBody)
{
	if (m_parent == removedBody) 
		m_parent = nullptr;
}

void Beam::TimeStepUpdate(const float timeStep)
{
	// Laser pulse's do not age well!
	m_age += timeStep;
	if (m_age > lifetime)
		Pi::game->GetSpace()->KillBody(this);
	SetPosition(GetPosition() + (m_baseVel * double(timeStep)));
}

float Beam::GetDamage() const
{
	return m_baseDam;
}

double Beam::GetRadius() const
{
	return sqrt(m_length*m_length);
}

static void MiningLaserSpawnTastyStuff(Frame *f, const SystemBody *asteroid, const vector3d &pos)
{
	lua_State *l = Lua::manager->GetLuaState();

	// lua cant push "const SystemBody", needs to convert to non-const
	RefCountedPtr<StarSystem> s = Pi::game->GetGalaxy()->GetStarSystem(asteroid->GetPath());
	SystemBody *liveasteroid = s->GetBodyByPath(asteroid->GetPath());

	// this is an adapted version of "CallMethod", because;
	// 1, there is no template for LuaObject<LuaTable>::CallMethod(..., SystemBody)
	// 2, this leaves the return value on the lua stack to be used by "new CargoBody()"
	LUA_DEBUG_START(l);
	LuaObject<Player>::PushToLua(Pi::player);
	lua_pushstring(l, "SpawnMiningContainer");
	lua_gettable(l, -2);
	lua_pushvalue(l, -2);
	lua_remove(l, -3);
	LuaObject<SystemBody>::PushToLua(liveasteroid);
	pi_lua_protected_call(l, 2, 1);

	CargoBody *cargo = new CargoBody(LuaRef(l, -1));
	lua_pop(l, 1);
	LUA_DEBUG_END(l, 0);

	cargo->SetFrame(f);
	cargo->SetPosition(pos);
	const double x = Pi::rng.Double();
	vector3d dir = pos.Normalized();
	dir.ArbRotate(vector3d(x, 1-x, 0), Pi::rng.Double()-.5);
	cargo->SetVelocity(Pi::rng.Double(100.0,200.0) * dir);
	Pi::game->GetSpace()->AddBody(cargo);
}

void Beam::StaticUpdate(const float timeStep)
{
	PROFILE_SCOPED()
	// This is just to stop it from hitting things repeatedly, it's dead in effect but still rendered
	if (!m_active)
		return;

	CollisionContact c;
	GetFrame()->GetCollisionSpace()->TraceRay(GetPosition(), m_dir.Normalized(), m_length, &c, static_cast<ModelBody*>(m_parent)->GetGeom());

	if (c.userData1) {
		Object *o = static_cast<Object*>(c.userData1);

		if (o->IsType(Object::CITYONPLANET)) {
			Pi::game->GetSpace()->KillBody(this);
		}
		else if (o->IsType(Object::BODY)) {
			Body *hit = static_cast<Body*>(o);
			if (hit != m_parent) {
				hit->OnDamage(m_parent, GetDamage(), c);
				m_active = false;
				if (hit->IsType(Object::SHIP))
					LuaEvent::Queue("onShipHit", dynamic_cast<Ship*>(hit), dynamic_cast<Body*>(m_parent));
			}
		}
	}

	if (m_mining) {
		// need to test for terrain hit
		if (GetFrame()->GetBody() && GetFrame()->GetBody()->IsType(Object::PLANET)) {
			Planet *const planet = static_cast<Planet*>(GetFrame()->GetBody());
			const SystemBody *b = planet->GetSystemBody();
			vector3d pos = GetPosition();
			double terrainHeight = planet->GetTerrainHeight(pos.Normalized());
			if (terrainHeight > pos.Length()) {
				// hit the fucker
				if (b->GetType() == SystemBody::TYPE_PLANET_ASTEROID) {
					vector3d n = GetPosition().Normalized();
					MiningLaserSpawnTastyStuff(planet->GetFrame(), b, n*terrainHeight + 5.0*n);
					SfxManager::Add(this, TYPE_EXPLOSION);
				}
				m_active = false;
			}
		}
	}
}

void Beam::Render(Graphics::Renderer *renderer, const Camera *camera, const vector3d &viewCoords, const matrix4x4d &viewTransform)
{
	PROFILE_SCOPED()
	const vector3d _from = viewTransform * GetInterpPosition();
	const vector3d _to = viewTransform * (GetInterpPosition() + (-m_dir));
	const vector3d _dir = _to - _from;
	const vector3f from(&_from.x);
	const vector3f dir = vector3f(_dir).Normalized();

	vector3f v1, v2;
	matrix4x4f m = matrix4x4f::Identity();
	v1.x = dir.y; v1.y = dir.z; v1.z = dir.x;
	v2 = v1.Cross(dir).Normalized();
	v1 = v2.Cross(dir);
	m[0] = v1.x; m[4] = v2.x; m[8] = dir.x;
	m[1] = v1.y; m[5] = v2.y; m[9] = dir.y;
	m[2] = v1.z; m[6] = v2.z; m[10] = dir.z;

	m[12] = from.x;
	m[13] = from.y;
	m[14] = from.z;

	// increase visible size based on distance from camera, z is always negative
	// allows them to be smaller while maintaining visibility for game play
	const float dist_scale = float(viewCoords.z / -500);
	const float length = m_length + dist_scale;
	const float width = 1.0f + dist_scale;

	renderer->SetTransform(m * matrix4x4f::ScaleMatrix(width, width, length));

	Color color = m_color;
	// fade them out as they age so they don't suddenly disappear
	// this matches the damage fall-off calculation
	const float base_alpha = 1.0f;
	// fade out side quads when viewing nearly edge on
	vector3f view_dir = vector3f(viewCoords).Normalized();
	color.a = (base_alpha * (1.f - powf(fabs(dir.Dot(view_dir)), length))) * 255;

	if (color.a > 3) {
		s_sideMat->diffuse = color;
		renderer->DrawTriangles(s_sideVerts.get(), s_renderState, s_sideMat.get());
	}

	// fade out glow quads when viewing nearly edge on
	// these and the side quads fade at different rates
	// so that they aren't both at the same alpha as that looks strange
	color.a = (base_alpha * powf(fabs(dir.Dot(view_dir)), width)) * 255;

	if (color.a > 3) {
		s_glowMat->diffuse = color;
		renderer->DrawTriangles(s_glowVerts.get(), s_renderState, s_glowMat.get());
	}
}

// static
void Beam::Add(Body *parent, const ProjectileData& prData, const vector3d &pos, const vector3d &baseVel, const vector3d &dir)
{
	Beam *p = new Beam();
	p->m_parent = parent;
	p->m_dir = dir;
	p->m_baseDam = prData.damage;
	p->m_length = prData.length;
	p->m_mining = prData.mining;
	p->m_color = prData.color;
	p->SetFrame(parent->GetFrame());

	p->SetOrient(parent->GetOrient());
	p->SetPosition(pos);
	p->m_baseVel = baseVel;
	p->SetClipRadius(p->GetRadius());
	p->SetPhysRadius(p->GetRadius());
	Pi::game->GetSpace()->AddBody(p);
}
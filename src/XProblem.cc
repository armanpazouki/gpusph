/*  Copyright 2011-2013 Alexis Herault, Giuseppe Bilotta, Robert A. Dalrymple, Eugenio Rustico, Ciro Del Negro

    Istituto Nazionale di Geofisica e Vulcanologia
        Sezione di Catania, Catania, Italy

    Università di Catania, Catania, Italy

    Johns Hopkins University, Baltimore, MD

    This file is part of GPUSPH.

    GPUSPH is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GPUSPH is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GPUSPH.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
#include <iostream>

// limits
#include <cfloat>
#include <limits>

#include "Rect.h"
#include "Disk.h"
#include "Cube.h"
#include "Cylinder.h"
#include "Cone.h"
#include "Sphere.h"
#include "Torus.h"
#include "Plane.h"
#include "STLMesh.h"
#include "XProblem.h"
#include "GlobalData.h"

//#define USE_PLANES 0

using namespace std;

XProblem::XProblem(GlobalData *_gdata) : Problem(_gdata)
{
	// *** XProblem initialization
	m_numActiveGeometries = 0;
	m_numForcesBodies = 0;
	m_numFloatingBodies = 0;
	m_numPlanes = 0;
	m_numOpenBoundaries = 0;

	m_numDynBoundLayers = 0;

	m_extra_world_margin = 0.0;

	m_positioning = PP_CENTER;

	// NAN water level and max fall: will autocompute if user doesn't define them
	m_waterLevel = NAN;
	m_maxFall = NAN;
	m_maxParticleSpeed = NAN;

	m_hydrostaticFilling = true;

	// *** Other parameters and settings
	m_name = "XProblem";

	// We don't initialize simparams because it will be done by the SETUP_FRAMEWORK
	// in the subclass

}

void XProblem::release_memory()
{
	m_fluidParts.clear();
	m_boundaryParts.clear();
	m_testpointParts.clear();
	// also cleanup object parts
	for (size_t g = 0, num_geoms = m_geometries.size(); g < num_geoms; g++) {
		if (m_geometries[g]->enabled)
			m_geometries[g]->ptr->GetParts().clear();
		if (m_geometries[g]->hdf5_reader)
			delete m_geometries[g]->hdf5_reader;
		if (m_geometries[g]->xyz_reader)
			delete m_geometries[g]->xyz_reader;
	}
}

uint XProblem::suggestedDynamicBoundaryLayers()
{
	return (uint)simparams()->get_influence_layers() + 1;
}

XProblem::~XProblem()
{
	release_memory();
	if (m_numFloatingBodies > 0)
		cleanupChrono();
}

bool XProblem::initialize()
{
	// setup the framework if the subclass did not do it; will have all defaults
	if (!simframework()) {
		// TODO automatic framework setup
		// This must be done in a CU file
		//SETUP_FRAMEWORK();
		throw runtime_error("no simulation framework defined");
	}

	// *** Add a writer, if none was specified
	if (get_writers().size() == 0)
		add_writer(VTKWRITER, 1e-2f);

	// *** Initialization of minimal physical parameters
	if (std::isnan(m_deltap))
		set_deltap(0.02f);
	if (std::isnan(physparams()->r0))
		physparams()->r0 = m_deltap;

	// aux vars to compute bounding box
	Point globalMin = Point(DBL_MAX, DBL_MAX, DBL_MAX);
	Point globalMax = Point(-DBL_MAX, -DBL_MAX, -DBL_MAX);

	// counters of floating objects and generic objects (floating + moving + open bounds)
	uint bodies_counter = 0;
	uint open_boundaries_counter = 0;

	// aux var for automatic water level computation
	double highest_water_part = NAN;

	for (size_t g = 0, num_geoms = m_geometries.size(); g < num_geoms; g++) {
		// aux vars to store bbox of current geometry
		Point currMin, currMax;

		// ignore planes for bbox
		if (m_geometries[g]->type == GT_PLANE)
			continue;

		// ignore deleted geometries
		if (!m_geometries[g]->enabled)
			continue;

		// load HDF5 files
		if (m_geometries[g]->has_hdf5_file)
			m_geometries[g]->hdf5_reader->read();
		else
		// load XYZ files and store their bounding box, at the same time
		if (m_geometries[g]->has_xyz_file)
			m_geometries[g]->xyz_reader->read(&currMin, &currMax);

		// geometries loaded from HDF5 files but not featuring a STL file do not have a
		// bounding box yet; let's compute it
		if (m_geometries[g]->has_hdf5_file && !m_geometries[g]->has_mesh_file) {

			// initialize temp variables
			currMin(0) = currMin(1) = currMin(2) = DBL_MAX;
			currMax(0) = currMax(1) = currMax(2) = -DBL_MAX;

			// iterate on particles - could printf something if long...
			for (uint p = 0; p < m_geometries[g]->hdf5_reader->getNParts(); p++) {
				// utility pointer & var
				ReadParticles *part = &(m_geometries[g]->hdf5_reader->buf[p]);
				Point currPoint(part->Coords_0, part->Coords_1, part->Coords_2);
				// set current per-coordinate minimum and maximum
				setMinPerElement(currMin, currPoint);
				setMaxPerElement(currMax, currPoint);
			}
			// TODO: store the so-computed bbox somewhere?
			// Not using it yet, but we have it for free here
		} else
		// points loaded from XYZ files already stored their bounding box; all other
		// geometries should have one ready
		if (!m_geometries[g]->has_xyz_file)
			m_geometries[g]->ptr->getBoundingBox(currMin, currMax);

		// global min and max
		setMinPerElement(globalMin, currMin);
		setMaxPerElement(globalMax, currMax);

		// store highest fluid part Z coordinate
		if (m_geometries[g]->type == GT_FLUID) {
			if (!isfinite(highest_water_part) || currMax(2) > highest_water_part)
				highest_water_part = currMax(2);
		}

		// update object counters
		if (m_geometries[g]->type == GT_FLOATING_BODY ||
			m_geometries[g]->type == GT_MOVING_BODY)
			bodies_counter++;
		if (m_geometries[g]->type == GT_OPENBOUNDARY)
			open_boundaries_counter++;
	}

	// here should be bodies_counter == m_numFloatingBodies
	if ( bodies_counter > MAX_BODIES ) {
		printf("Fatal: number of bodies > MAX_BODIES (%u > %u)\n",
			bodies_counter, MAX_BODIES);
		return false;
	}

	// do not store the number of floating objects in simparams: add_moving_body()
	// will increment it and use it for the insertion in the vector

	// store number of objects (floating + moving + I/O)
	simparams()->numOpenBoundaries = open_boundaries_counter;

	// Increase the world dimensions of a m_deltap quantity. This is necessary
	// to guarantee a distance of m_deltap between particles of either sides of
	// periodic boundaries; moreover, for boundaries without any periodicity
	// it ensures that all particles are within the domain even in case of
	// numerical rounding errors.
	globalMin(0) -= (m_deltap/2);
	globalMax(0) += (m_deltap/2);
	globalMin(1) -= (m_deltap/2);
	globalMax(1) += (m_deltap/2);
	globalMin(2) -= (m_deltap/2);
	globalMax(2) += (m_deltap/2);

	// compute the number of layers for dynamic boundaries, if not set
	if (simparams()->boundarytype == DYN_BOUNDARY && m_numDynBoundLayers == 0) {
		m_numDynBoundLayers = suggestedDynamicBoundaryLayers();
		printf("Number of dynamic boundary layers not set, autocomputed: %u\n", m_numDynBoundLayers);
	}

	// Increase the world dimensions for dinamic boundaries in directions without periodicity
	if (simparams()->boundarytype == DYN_BOUNDARY){
		if (!(simparams()->periodicbound & PERIODIC_X)){
			globalMin(0) -= (m_numDynBoundLayers-1)*m_deltap;
			globalMax(0) += (m_numDynBoundLayers-1)*m_deltap;
		}
		if (!(simparams()->periodicbound & PERIODIC_Y)){
			globalMin(1) -= (m_numDynBoundLayers-1)*m_deltap;
			globalMax(1) += (m_numDynBoundLayers-1)*m_deltap;
		}
		if (!(simparams()->periodicbound & PERIODIC_Z)){
			globalMin(2) -= (m_numDynBoundLayers-1)*m_deltap;
			globalMax(2) += (m_numDynBoundLayers-1)*m_deltap;
		}
	}

	// set computed world origin and size without overriding possible user choices
	if (!isfinite(m_origin.x)) m_origin.x = globalMin(0);
	if (!isfinite(m_origin.y)) m_origin.y = globalMin(1);
	if (!isfinite(m_origin.z)) m_origin.z = globalMin(2);
	if (!isfinite(m_size.x)) m_size.x = globalMax(0) - globalMin(0);
	if (!isfinite(m_size.y)) m_size.y = globalMax(1) - globalMin(1);
	if (!isfinite(m_size.z)) m_size.z = globalMax(2) - globalMin(2);

	// add user-defined world margin, if any
	if (m_extra_world_margin > 0.0) {
		m_origin -= m_extra_world_margin;
		m_size += 2 * m_extra_world_margin;
	}

	// compute water level automatically, if not set
	if (!isfinite(m_waterLevel)) {
		// water level: highest fluid coordinate or (absolute) domain height
		m_waterLevel = ( isfinite(highest_water_part) ? highest_water_part : m_size.z - m_origin.z );
		printf("Water level not set, autocomputed: %g\n", m_waterLevel);
	}

	// ditto for max fall; approximated as (waterLevel - lowest_domain_point)
	// NOTE: if there is no fluid geometry and both water level and maxFall are autocomputed, then
	// water level will be equal to highest domain point and max fall to domain height
	if (!isfinite(m_maxFall)) {
		m_maxFall = m_waterLevel - globalMin(2);
		printf("Max fall height not set, autocomputed: %g\n", m_maxFall);
	}

	// set physical parameters depending on m_maxFall or m_waterLevel: LJ dcoeff, sspeed (through set_density())
	const float g = length(physparams()->gravity);
	physparams()->dcoeff = 5.0f * g * m_maxFall;

	// Disable hydrostatic filling if there is no gravity
	if (g == 0)
		m_hydrostaticFilling = false;

	if (!isfinite(m_maxParticleSpeed)) {
		m_maxParticleSpeed = sqrt(2.0 * g * m_maxFall);
		printf("Max particle speed not set, autocomputed from max fall: %g\n", m_maxParticleSpeed);
	}

	const float default_rho = 1000.0;
	const float default_kinematic_visc = 1.0e-6f;
	const float default_gamma = 7;
	// numerical speed of sound TODO multifluid
	const float default_c0 = 10.0 * m_maxParticleSpeed;

	if (physparams()->numFluids() == 0) {
		physparams()->add_fluid(default_rho);
		printf("No fluids specified, assuming water (rho: %g\n",
			default_rho);
	}

	for (size_t fluid = 0 ; fluid < physparams()->numFluids(); ++fluid) {
		const bool must_set_gamma = std::isnan(physparams()->gammacoeff[fluid]);
		const bool must_set_c0 = std::isnan(physparams()->sscoeff[fluid]);

		// tell the user what we're going to do
		if (must_set_gamma && must_set_c0) {
			printf("EOS for fluid %zu not specified, assuming water (gamma: %g, c0: %g)\n",
				fluid, default_gamma, default_c0);
		} else if (must_set_c0) {
			printf("Speed of sound for fluid %zu auto-computed as c0 = %g\n",
				fluid, default_c0);
		} else if (must_set_gamma) {
			// we consider this an anomalous situation, since gamma should always
			// be specified if c0 was, hence stderr
			fprintf(stderr, "Incomplete EOS for fluid %zu, assuming water (gamma: %g)\n",
				fluid, default_gamma);
		}

		// set the EOS if needed
		if (must_set_gamma || must_set_c0)
			physparams()->set_equation_of_state(
				fluid,
				must_set_gamma ? default_gamma : physparams()->gammacoeff[fluid],
				must_set_c0 ? default_c0 : physparams()->sscoeff[fluid]);

		// set the viscosity if needed
		if (std::isnan(physparams()->kinematicvisc[fluid])) {
			printf("Viscosity for fluid %zu not specified, assuming water (nu = %g)\n",
				fluid, default_kinematic_visc);
			physparams()->set_kinematic_visc(fluid, default_kinematic_visc);
		}
	}

	// only init Chrono if there are floating bodies
	if (m_numFloatingBodies)
		initializeChrono();

	// check open boundaries consistency
	// TODO ideally we should enable/disable them depending on whether
	// they are present, but this isn't trivial to do with the static framework
	// options
	if (m_numOpenBoundaries > 0 && !(simparams()->simflags & ENABLE_INLET_OUTLET))
		throw invalid_argument("open boundaries present, but ENABLE_INLET_OUTLET not specified in framework flag");
	if (m_numOpenBoundaries == 0 && (simparams()->simflags & ENABLE_INLET_OUTLET))
		throw invalid_argument("no open boundaries present, but ENABLE_INLET_OUTLET specified in framework flag");

	// TODO FIXME m_numMovingObjects does not exist yet
	//if (m_numMovingObjects > 0)
	//	simparams()->movingBoundaries = true;

	// Call Problem's initialization that takes care of the common
	// initialization functions (checking dt, preparing the grid,
	// creating the problem dir, etc)
	return Problem::initialize();
}

void XProblem::initializeChrono()
{
#if USE_CHRONO == 1
	InitializeChrono();
#endif
}

void XProblem::cleanupChrono()
{
#if USE_CHRONO == 1
	FinalizeChrono();
#endif
}

GeometryID XProblem::addGeometry(const GeometryType otype, const FillType ftype, Object* obj_ptr,
	const char *hdf5_fname, const char *xyz_fname, const char *stl_fname)
{
	// TODO: before even creating the new GeometryInfo we should check the compatibility of
	// the combination of paramenters (e.g. no moving planes; no HDF5 testpoints)
	GeometryInfo* geomInfo = new GeometryInfo();
	geomInfo->type = otype;
	geomInfo->fill_type = ftype;
	geomInfo->ptr = obj_ptr;
	if (hdf5_fname) {
		geomInfo->hdf5_filename = string(hdf5_fname);
		geomInfo->has_hdf5_file = true;
		// initialize the reader
		// TODO: error checking
		geomInfo->hdf5_reader = new HDF5SphReader();
		geomInfo->hdf5_reader->setFilename(hdf5_fname);
	} else
	if (xyz_fname) {
		geomInfo->xyz_filename = string(xyz_fname);
		geomInfo->has_xyz_file = true;
		// initialize the reader
		// TODO: error checking
		geomInfo->xyz_reader = new XYZReader();
		geomInfo->xyz_reader->setFilename(xyz_fname);
	}
	if (stl_fname) {
		geomInfo->stl_filename = string(stl_fname);
		geomInfo->has_mesh_file = true;
	}
	m_numActiveGeometries++;

	// --- Default collision and dynamics
	switch (geomInfo->type) {
		case GT_FLUID:
			geomInfo->handle_collisions = false;
			geomInfo->handle_dynamics = false;
			geomInfo->measure_forces = false;
			break;
		case GT_FIXED_BOUNDARY:
			geomInfo->handle_collisions = true; // optional
			geomInfo->handle_dynamics = false;
			geomInfo->measure_forces = false;
			break;
		case GT_OPENBOUNDARY:
			geomInfo->handle_collisions = false; // TODO: make optional?
			geomInfo->handle_dynamics = false;
			geomInfo->measure_forces = false; // TODO: make optional?
			break;
		case GT_FLOATING_BODY:
			geomInfo->handle_collisions = true; // optional
			geomInfo->handle_dynamics = true;
			geomInfo->measure_forces = true;
			break;
		case GT_MOVING_BODY:
			geomInfo->handle_collisions = true; // optional
			geomInfo->handle_dynamics = false; // optional
			geomInfo->measure_forces = false; // optional
			break;
		case GT_PLANE:
			geomInfo->handle_collisions = true; // optional
			geomInfo->handle_dynamics = false;
			geomInfo->measure_forces = false;
			break;
		case GT_TESTPOINTS:
			geomInfo->handle_collisions = false;
			geomInfo->handle_dynamics = false;
			geomInfo->measure_forces = false;
			break;
	}

	// --- Default intersection type
	// It is IT_SUBTRACT by default, except for planes: they are usually used
	// to delimit the boundaries of the domain, so we likely want to intersect
	switch (geomInfo->type) {
		case GT_PLANE:
			geomInfo->intersection_type = IT_INTERSECT;
			break;
		case GT_TESTPOINTS:
			geomInfo->intersection_type = IT_NONE;
			break;
		default:
			geomInfo->intersection_type = IT_SUBTRACT;
	}

	// --- Default erase operation
	// Upon intersection or subtraction we can choose to interact with fluid
	// or boundaries. By default, water erases only other water, while boundaries
	// erase water and other boundaries. Testpoints eras nothing.
	switch (geomInfo->type) {
		case GT_FLUID:
			geomInfo->erase_operation = ET_ERASE_FLUID;
			break;
		case GT_TESTPOINTS:
			geomInfo->erase_operation = ET_ERASE_NOTHING;
			break;
		default:
			geomInfo->erase_operation = ET_ERASE_ALL;
	}

	// NOTE: we don't need to check handle_collisions at all if there are no bodies
	if (geomInfo->handle_dynamics)
		m_numFloatingBodies++;

	if (geomInfo->measure_forces)
		m_numForcesBodies++;

	if (geomInfo->type == GT_PLANE)
		m_numPlanes++;

	if (geomInfo->type == GT_OPENBOUNDARY)
		m_numOpenBoundaries++;

	m_geometries.push_back(geomInfo);
	return (m_geometries.size() - 1);
}

bool XProblem::validGeometry(GeometryID gid)
{
	// ensure gid refers to a valid position
	if (gid >= m_geometries.size()) {
		printf("WARNING: invalid GeometryID %zu\n", gid);
		return false;
	}

	// ensure geometry was not deleted
	if (!m_geometries[gid]->enabled) {
		printf("WARNING: GeometryID %zu refers to a deleted geometry!\n", gid);
		return false;
	}

	return true;
}

GeometryID XProblem::addRect(const GeometryType otype, const FillType ftype, const Point &origin,
	const double side1, const double side2)
{
	double offsetX = 0, offsetY = 0;
	if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
		offsetX = - side1 / 2.0;
		offsetY = - side2 / 2.0;
	}

	return addGeometry(otype, ftype,
		new Rect( Point( origin(0) + offsetX, origin(1) + offsetY, origin(2) ),
			side1, side2, EulerParameters() )
	);
}

GeometryID XProblem::addDisk(const GeometryType otype, const FillType ftype, const Point &origin,
	const double radius)
{
	double offsetX = 0, offsetY = 0;
	if (m_positioning == PP_CORNER)
		offsetX = offsetY = radius;

	return addGeometry(otype, ftype,
		new Disk( Point( origin(0) + offsetX, origin(1) + offsetY, origin(2) ),
			radius, EulerParameters() )
	);
}

GeometryID XProblem::addCube(const GeometryType otype, const FillType ftype, const Point &origin, const double side)
{
	double offsetXY = 0, offsetZ = 0;
	if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER)
		offsetXY = - side / 2.0;
	if (m_positioning == PP_CENTER)
		offsetZ = - side / 2.0;

	return addGeometry(otype, ftype,
		new Cube( Point( origin(0) + offsetXY, origin(1) + offsetXY, origin(2) + offsetZ ),
			side, side, side, EulerParameters() )
	);
}

GeometryID XProblem::addBox(const GeometryType otype, const FillType ftype, const Point &origin,
			const double side1, const double side2, const double side3)
{
	double offsetX = 0, offsetY = 0, offsetZ = 0;
	if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
		offsetX = - side1 / 2.0;
		offsetY = - side2 / 2.0;
	}
	if (m_positioning == PP_CENTER)
		offsetZ = - side3 / 2.0;

	return addGeometry(otype, ftype,
		new Cube( Point( origin(0) + offsetX, origin(1) + offsetY, origin(2) + offsetZ ),
			side1, side2, side3, EulerParameters() )
	);
}

GeometryID XProblem::addCylinder(const GeometryType otype, const FillType ftype, const Point &origin,
			const double radius, const double height)
{
	double offsetXY = 0, offsetZ = 0;
	if (m_positioning == PP_CORNER)
		offsetXY = radius;
	else
	if (m_positioning == PP_CENTER)
		offsetZ = - height / 2.0;

	return addGeometry(otype, ftype,
		new Cylinder( Point( origin(0) + offsetXY, origin(1) + offsetXY, origin(2) + offsetZ ),
			radius, height, EulerParameters() )
	);
}

GeometryID XProblem::addCone(const GeometryType otype, const FillType ftype, const Point &origin,
	const double bottom_radius, const double top_radius, const double height)
{
	double offsetXY = 0, offsetZ = 0;
	if (m_positioning == PP_CORNER)
		offsetXY = bottom_radius;
	else
	if (m_positioning == PP_CENTER) {
		// height of the geometrical center of the truncated cone (round frustum)
		offsetZ = - height *
			(bottom_radius * bottom_radius + 2.0 * bottom_radius * top_radius + 3.0 * top_radius * top_radius)/
			(4.0 * (bottom_radius * bottom_radius + bottom_radius * top_radius + top_radius * top_radius));
		// center of the bounding box
		// offsetZ = height / 2.0;
	}

	return addGeometry(otype, ftype,
		new Cone( Point( origin(0) + offsetXY, origin(1) + offsetXY, origin(2) + offsetZ ),
			bottom_radius, top_radius, height, EulerParameters() )
	);
}

GeometryID XProblem::addSphere(const GeometryType otype, const FillType ftype, const Point &origin,
	const double radius)
{
	double offsetXY = 0, offsetZ = 0;
	if (m_positioning == PP_CORNER || m_positioning == PP_BOTTOM_CENTER)
		offsetZ = radius;
	if (m_positioning == PP_CORNER)
		offsetXY = radius;

	return addGeometry(otype, ftype,
		new Sphere( Point( origin(0) + offsetXY, origin(1) + offsetXY, origin(2) + offsetZ ),
			radius )
	);
}

GeometryID XProblem::addTorus(const GeometryType otype, const FillType ftype, const Point &origin,
	const double major_radius, const double minor_radius)
{
	double offsetXY = 0, offsetZ = 0;
	if (m_positioning == PP_CORNER || m_positioning == PP_BOTTOM_CENTER)
		offsetZ = minor_radius;
	if (m_positioning == PP_CORNER)
		offsetXY = (major_radius + minor_radius);

	return addGeometry(otype, ftype,
		new Torus( origin, major_radius, minor_radius, EulerParameters() )
	);
}

GeometryID XProblem::addPlane(
	const double a_coeff, const double b_coeff, const double c_coeff, const double d_coeff)
{
	return addGeometry(GT_PLANE, FT_NOFILL,
		new Plane( a_coeff, b_coeff, c_coeff, d_coeff )
	);
}

// NOTE: "origin" has a slightly different meaning than for the other primitives: here it is actually
// an offset to shift the STL coordinates. Use 0 to import STL coords as they are.
// If positioning is PP_NONE and origin is (0,0,0), mesh coordinates are imported unaltered.
GeometryID XProblem::addSTLMesh(const GeometryType otype, const FillType ftype, const Point &origin,
	const char *filename)
{
	STLMesh *stlmesh = STLMesh::load_stl(filename);

	double offsetX = 0, offsetY = 0, offsetZ = 0;

	// handle positioning
	if (m_positioning != PP_NONE) {

		// Make the origin coincide with the lower corner of the mesh bbox.
		// Now the positioning is PP_CORNER
		stlmesh->shift( - stlmesh->get_minbounds() );

		// NOTE: STLMesh::get_meshsize() returns #triangles instead
		const double3 mesh_size = stlmesh->get_maxbounds() - stlmesh->get_minbounds();

		if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
			offsetX = - mesh_size.x / 2.0;
			offsetY = - mesh_size.y / 2.0;
		}

		if (m_positioning == PP_CENTER) {
			offsetZ = - mesh_size.z / 2.0;
		}

	} // if positioning is PP_NONE

	// shift STL origin to given point
	stlmesh->shift( make_double3(origin(0) + offsetX, origin(1) + offsetY, origin(2) + offsetZ) );

	return addGeometry(
		otype,
		ftype,
		stlmesh,
		NULL,			// HDF5 filename
		NULL,			// XYZ filename
		filename		// STL filename
	);
}

// NOTE: "origin" has a slightly different meaning than for the other primitives: here it is actually
// an offset to shift the STL coordinates. Use 0 to import STL coords as they are.
// If positioning is PP_NONE and origin is (0,0,0), mesh coordinates are imported unaltered.
GeometryID XProblem::addOBJMesh(const GeometryType otype, const FillType ftype, const Point &origin,
	const char *filename)
{
	STLMesh *stlmesh = new STLMesh();
	stlmesh->setObjectFile(filename);
	stlmesh->loadObjBounds(); // update bbox

	double offsetX = 0, offsetY = 0, offsetZ = 0;

	// handle positioning
	if (m_positioning != PP_NONE) {

		// Make the origin coincide with the lower corner of the mesh bbox.
		// Now the positioning is PP_CORNER
		stlmesh->shift( - stlmesh->get_minbounds() );

		// NOTE: STLMesh::get_meshsize() returns #triangles instead
		const double3 mesh_size = stlmesh->get_maxbounds() - stlmesh->get_minbounds();

		if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
			offsetX = - mesh_size.x / 2.0;
			offsetY = - mesh_size.y / 2.0;
		}

		if (m_positioning == PP_CENTER) {
			offsetZ = - mesh_size.z / 2.0;
		}

	} // if positioning is PP_NONE

	// shift STL origin to given point
	stlmesh->shift( make_double3(origin(0) + offsetX, origin(1) + offsetY, origin(2) + offsetZ) );

	return addGeometry(
		otype,
		ftype,
		stlmesh,
		NULL,			// HDF5 filename
		NULL,			// XYZ filename
		filename		// STL filename
	);
}


// NOTE: particles loaded from HDF5 files will not be erased!
// To enable erase-like interaction we need to copy them to the particle vectors, which
// requires unnecessary memory allocation
GeometryID XProblem::addHDF5File(const GeometryType otype, const Point &origin,
	const char *fname_hdf5, const char *fname_obj)
{
	// NOTES about HDF5 files
	// - fill type is FT_NOFILL since particles are read from file
	// - may add a null STLMesh if the hdf5 file is given but not the mesh
	// - should adding an HDF5 file of type GT_TESTPOINTS be forbidden?

	// will create anyway an empty STLMesh if the STL filename is not given
	STLMesh *stlmesh = new STLMesh();
	if (fname_obj) {
		stlmesh->setObjectFile(fname_obj);
		stlmesh->loadObjBounds(); // update bbox

		double offsetX = 0, offsetY = 0, offsetZ = 0;

		// handle positioning
		if (m_positioning != PP_NONE) {

			// Make the origin coincide with the lower corner of the mesh bbox.
			// Now the positioning is PP_CORNER
			stlmesh->shift( - stlmesh->get_minbounds() );

			// NOTE: STLMesh::get_meshsize() returns #triangles instead
			const double3 mesh_size = stlmesh->get_maxbounds() - stlmesh->get_minbounds();

			if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
				offsetX = - mesh_size.x / 2.0;
				offsetY = - mesh_size.y / 2.0;
			}

			if (m_positioning == PP_CENTER) {
				offsetZ = - mesh_size.z / 2.0;
			}

		} // if positioning is PP_NONE

		// shift STL origin to given point
		stlmesh->shift( make_double3(origin(0) + offsetX, origin(1) + offsetY, origin(2) + offsetZ) );
	}

	// NOTE: an empty STL mesh does not return a meaningful bounding box. Will read parts in initialize() for that

	return addGeometry(
		otype,
		FT_NOFILL,
		stlmesh,
		fname_hdf5,			// HDF5 filename
		NULL,			// XYZ filename
		fname_obj		// STL filename
	);
}

// NOTE: particles loaded from XYZ files will not be erased!
// To enable erase-like interaction we need to copy them to the global particle vectors, by passing an
// existing vector to loadPointCloudFromXYZFile(). We can implement this if needed.
GeometryID XProblem::addXYZFile(const GeometryType otype, const Point &origin,
			const char *fname_xyz, const char *fname_obj)
{
	// NOTE: fill type is FT_NOFILL since particles are read from file

	// create an empty STLMesh if a mesh filename is not given
	STLMesh *stlmesh = new STLMesh(0);
	if (fname_obj)
		stlmesh->setObjectFile(fname_obj);

	double offsetX = 0, offsetY = 0, offsetZ = 0;

	// handle positioning
	if (m_positioning != PP_NONE) {

		// Make the origin coincide with the lower corner of the mesh bbox.
		// Now the positioning is PP_CORNER
		stlmesh->shift( - stlmesh->get_minbounds() );

		// NOTE: STLMesh::get_meshsize() returns #triangles instead
		const double3 mesh_size = stlmesh->get_maxbounds() - stlmesh->get_minbounds();

		if (m_positioning == PP_CENTER || m_positioning == PP_BOTTOM_CENTER) {
			offsetX = - mesh_size.x / 2.0;
			offsetY = - mesh_size.y / 2.0;
		}

		if (m_positioning == PP_CENTER) {
			offsetZ = - mesh_size.z / 2.0;
		}

	} // if positioning is PP_NONE

	// shift STL origin to given point
	stlmesh->shift( make_double3(origin(0) + offsetX, origin(1) + offsetY, origin(2) + offsetZ) );

	// NOTE: an empty STL mesh does not return a meaningful bounding box. Will read parts for that

	return addGeometry(
		otype,
		FT_NOFILL,
		stlmesh,
		NULL,			// HDF5 filename
		fname_xyz,		// XYZ filename
		fname_obj		// STL filename
	);
}

// Add a single testpoint; returns the position of the testpoint in the vector of
// testpoints, which will correspond to its particle id.
// NOTE: testpoints should be assigned with consecutive particle ids starting from 0
size_t XProblem::addTestPoint(const Point &coordinates)
{
	m_testpointParts.push_back(coordinates);
	return (m_testpointParts.size() - 1);
}

// Simple overload
size_t XProblem::addTestPoint(const double posx, const double posy, const double posz)
{
	return addTestPoint(Point(posx, posy, posz));
}

// request to invert normals while loading - only for HDF5 files
void XProblem::flipNormals(const GeometryID gid, bool flip)
{
	if (!validGeometry(gid)) return;

	// this makes sense only for geometries loading a HDF5 file
	// TODO: also enable for planes?
	if (!m_geometries[gid]->has_hdf5_file) {
		printf("WARNING: trying to invert normals on a geometry without HDF5-files associated! Ignoring\n");
		return;
	}

	m_geometries[gid]->flip_normals = flip;
}

void XProblem::deleteGeometry(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	m_geometries[gid]->enabled = false;

	// and this is the reason why m_numActiveGeometries not be used to iterate on m_geometries:
	m_numActiveGeometries--;

	if (m_geometries[gid]->measure_forces)
		m_numForcesBodies--;

	if (m_geometries[gid]->handle_dynamics)
		m_numFloatingBodies--;

	if (m_geometries[gid]->type == GT_PLANE)
		m_numPlanes--;

	if (m_geometries[gid]->type == GT_OPENBOUNDARY)
		m_numOpenBoundaries--;

	// TODO: print a warning if deletion is requested after fill_parts
}

void XProblem::enableDynamics(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// ensure dynamics are consistent with geometry type
	if (m_geometries[gid]->type != GT_FLOATING_BODY &&
		m_geometries[gid]->type != GT_MOVING_BODY) {
		printf("WARNING: dynamics only available for rigid bodies! Ignoring\n");
		return;
	}
	// update counter of rigid bodies if needed
	if (!m_geometries[gid]->handle_dynamics)
		m_numFloatingBodies++;
	m_geometries[gid]->handle_dynamics = true;
}

void XProblem::enableCollisions(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// TODO: allow collisions for open boundaries? Why not for fixed bounds?
	// ensure collisions are consistent with geometry type
	if (m_geometries[gid]->type != GT_FLOATING_BODY &&
		m_geometries[gid]->type != GT_MOVING_BODY &&
		m_geometries[gid]->type != GT_PLANE) {
		printf("WARNING: collisions only available for rigid bodies and planes! Ignoring\n");
		return;
	}
	m_geometries[gid]->handle_collisions = true;
}

void XProblem::disableDynamics(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// ensure no-dynamics is consistent with geometry type
	if (m_geometries[gid]->type == GT_FLOATING_BODY) {
		printf("WARNING: dynamics are mandatory for floating bodies! Ignoring\n");
		return;
	}
	// update counter of rigid bodies if needed
	if (m_geometries[gid]->handle_dynamics)
		m_numFloatingBodies--;
	m_geometries[gid]->handle_dynamics = false;
}

void XProblem::disableCollisions(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// it is possible to disable collisions for any geometry type, so no need to check it
	m_geometries[gid]->handle_collisions = false;
}

void XProblem::enableFeedback(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// TODO: allow collisions for open boundaries? Why not for fixed bounds?
	// ensure collisions are consistent with geometry type
	if (m_geometries[gid]->type != GT_FLOATING_BODY &&
		m_geometries[gid]->type != GT_MOVING_BODY) {
		printf("WARNING: feedback only available for floating or moving bodies! Ignoring\n");
		return;
	}

	if (!m_geometries[gid]->measure_forces)
		m_numForcesBodies++;
	m_geometries[gid]->measure_forces = true;
}

void XProblem::disableFeedback(const GeometryID gid)
{
	if (!validGeometry(gid)) return;

	// ensure no-dynamics is consistent with geometry type
	if (m_geometries[gid]->type == GT_FLOATING_BODY) {
		printf("WARNING: feedback is mandatory for floating bodies! Ignoring\n");
		return;
	}

	if (m_geometries[gid]->measure_forces)
		m_numForcesBodies--;
	m_geometries[gid]->measure_forces = false;
}

// Set a custom inertia matrix (main diagonal only). Will overwrite the precomputed one
void XProblem::setInertia(const GeometryID gid, const double i11, const double i22, const double i33)
{
	if (!validGeometry(gid)) return;

	// implicitly checking that geometry is a GT_FLOATING_BODY
	if (!m_geometries[gid]->handle_dynamics) {
		printf("WARNING: trying to set inertia of a geometry with no dynamics! Ignoring\n");
		return;
	}
	m_geometries[gid]->custom_inertia[0] = i11;
	m_geometries[gid]->custom_inertia[1] = i22;
	m_geometries[gid]->custom_inertia[2] = i33;
}

// overload
void XProblem::setInertia(const GeometryID gid, const double* mainDiagonal)
{
	setInertia(gid, mainDiagonal[0], mainDiagonal[1], mainDiagonal[2]);
}

// NOTE: GPUSPH uses ZXZ angles counterclockwise, ODE used XYZ clockwise (http://goo.gl/bV4Zeb - http://goo.gl/oPnMCv)
// We should check what's used by Chrono
void XProblem::setOrientation(const GeometryID gid, const EulerParameters &ep)
{
	if (!validGeometry(gid)) return;

	m_geometries[gid]->ptr->setEulerParameters(ep);
}

// DEPRECATED until we'll have a GPUSPH Quaternion class
void XProblem::rotate(const GeometryID gid, const EulerParameters ep)
{
	if (!validGeometry(gid)) return;

	//m_geometries[gid]->ptr->setEulerParameters(ep * (*(m_geometries[gid]->ptr->getEulerParameters())) );
	m_geometries[gid]->ptr->setEulerParameters(ep * m_geometries[gid]->ptr->getEulerParameters());
}

// NOTE: rotates X first, then Y, then Z
void XProblem::rotate(const GeometryID gid, const double Xrot, const double Yrot, const double Zrot)
{
	if (!validGeometry(gid)) return;

	// compute single-axes rotations
	double4 rotX, rotY, rotZ;
	// TODO: check if Chrono uses clockwise (like ODE did) or counterclockwise angles for Euler
	// NOTE: with ODE we computed the quaternions ourselves for accuracy reasons:
	// for each rotation, the real part of the quaternion is cos(angle/2),
	// and the imaginary part (which for rotations around principal axis
	// is only 1, 2 or 3) is sin(angle/2); the rest of the components are 0.
	// TODO: check if we need it with Chrono as well
	rotX.x = cos(-Xrot/2); rotX.y = sin(-Xrot/2); rotX.z = rotX.w = 0;
	rotY.x = cos(-Yrot/2); rotY.z = sin(-Yrot/2); rotY.y = rotY.w = 0;
	rotZ.x = cos(-Zrot/2); rotZ.w = sin(-Zrot/2); rotZ.y = rotZ.z = 0;
	// Problem: even with a “nice” angle such as M_PI we might end up
	// with not-exactly-zero components, so we kill anything which is less
	// than half the double-precision machine epsilon. If you REALLY care
	// about angles that differ from quadrant angles by less than 2^-53,
	// sorry, we don't have enough accuracy for you.
	if (fabs(rotX.x) < DBL_EPSILON/2)
		rotX.x = 0;
	if (fabs(rotX.y) < DBL_EPSILON/2)
		rotX.y = 0;
	if (fabs(rotY.x) < DBL_EPSILON/2)
		rotY.x = 0;
	if (fabs(rotY.z) < DBL_EPSILON/2)
		rotY.z = 0;
	if (fabs(rotZ.x) < DBL_EPSILON/2)
		rotZ.x = 0;
	if (fabs(rotZ.w) < DBL_EPSILON/2)
		rotZ.w = 0;

	// concatenate rotations in order (X, Y, Z)
	EulerParameters rotXY = EulerParameters(rotY.x, rotY.y, rotY.z, rotY.w) * EulerParameters(rotX.x, rotX.y, rotX.z, rotX.w);
	EulerParameters rotXYZ = EulerParameters(rotZ.x, rotZ.y, rotZ.z, rotZ.w) * rotXY;

	// rotate with computed quaternion
	rotate( gid, rotXYZ );
}

void XProblem::shift(const GeometryID gid, const double Xoffset, const double Yoffset, const double Zoffset)
{
	if (!validGeometry(gid)) return;

	if (m_geometries[gid]->type == GT_PLANE) {
		printf("WARNING: shift is not available for planes! Ignoring\n");
		return;
	}

	m_geometries[gid]->ptr->shift(make_double3(Xoffset, Yoffset, Zoffset));
}

void XProblem::setIntersectionType(const GeometryID gid, IntersectionType i_type)
{
	if (!validGeometry(gid)) return;

	m_geometries[gid]->intersection_type = i_type;
}

void XProblem::setEraseOperation(const GeometryID gid, EraseOperation e_operation)
{
	if (!validGeometry(gid)) return;

	m_geometries[gid]->erase_operation = e_operation;
}

void XProblem::setMass(const GeometryID gid, const double mass)
{
	if (!validGeometry(gid)) return;

	if (m_geometries[gid]->type != GT_FLOATING_BODY)
		printf("WARNING: setting mass of a non-floating body\n");
	m_geometries[gid]->ptr->SetMass(mass);
	m_geometries[gid]->mass_was_set = true;
}

double XProblem::setMassByDensity(const GeometryID gid, const double density)
{
	if (!validGeometry(gid)) return NAN;

	if (m_geometries[gid]->type != GT_FLOATING_BODY)
		printf("WARNING: setting mass of a non-floating body\n");

	const double mass = m_geometries[gid]->ptr->SetMass(physparams()->r0, density);
	m_geometries[gid]->mass_was_set = true;

	return mass;
}

void XProblem::setParticleMass(const GeometryID gid, const double mass)
{
	if (!validGeometry(gid)) return;

	// TODO: if SA bounds && geom is not fluid, throw exception or print warning

	m_geometries[gid]->ptr->SetPartMass(mass);
	m_geometries[gid]->particle_mass_was_set = true;
}

double XProblem::setParticleMassByDensity(const GeometryID gid, const double density)
{
	if (!validGeometry(gid)) return NAN;

	if ( (m_geometries[gid]->has_xyz_file || m_geometries[gid]->has_hdf5_file) &&
		 !m_geometries[gid]->has_mesh_file)
		printf("WARNING: setting the mass by density can't work with a point-based geometry without a mesh!\n");

	const double dx = (m_geometries[gid]->type == GT_FLUID ? m_deltap : physparams()->r0);
	const double particle_mass = m_geometries[gid]->ptr->SetPartMass(dx, density);
	m_geometries[gid]->particle_mass_was_set = true;

	return particle_mass;
}

// Flag an open boundary as velocity driven, so its particles will be flagged with
// as VEL_IO during fill. Use with false to revert to pressure driven.
// Only makes sense with GT_OPENBOUNDARY geometries.
void XProblem::setVelocityDriven(const GeometryID gid, bool isVelocityDriven)
{
	if (!validGeometry(gid)) return;

	if (m_geometries[gid]->type != GT_OPENBOUNDARY) {
		printf("WARNING: trying to set as velocity driven a non-GT_OPENBOUNDARY geometry! Ignoring\n");
		return;
	}

	m_geometries[gid]->velocity_driven = isVelocityDriven;
}

// Set custom radius for unfill operations. NAN means: use dp
void XProblem::setUnfillRadius(const GeometryID gid, double unfillRadius)
{
	if (!validGeometry(gid)) return;

	m_geometries[gid]->unfill_radius = unfillRadius;
}

const GeometryInfo* XProblem::getGeometryInfo(GeometryID gid)
{
	// NOTE: not checking validGeometry() to allow for deleted geometries

	// ensure gid refers to a valid position
	if (gid >= m_geometries.size()) {
		printf("WARNING: invalid GeometryID %zu\n", gid);
		return NULL;
	}

	// return geometry even if deleted
	return m_geometries[gid];
}

// set the positioning policy for geometries added after the call
void XProblem::setPositioning(PositioningPolicy positioning)
{
	m_positioning = positioning;
}

// Create 6 planes delimiting the box defined by the two points and update (overwrite) the world origin and size.
// Write their GeometryIDs in planesIds, if given, so that it is possible to delete one or more of them afterwards.
vector<GeometryID> XProblem::makeUniverseBox(const double3 corner1, const double3 corner2)
{
	vector<GeometryID> planes;

	// compute min and max
	double3 min, max;
	min.x = fmin(corner1.x, corner2.x);
	min.y = fmin(corner1.y, corner2.y);
	min.z = fmin(corner1.z, corner2.z);
	max.x = fmax(corner1.x, corner2.x);
	max.y = fmax(corner1.y, corner2.y);
	max.z = fmax(corner1.z, corner2.z);

	// we need the periodicity to see which planes are needed. If simparams() is NULL,
	// it means SETUP_FRAMEWORK was not invoked, in which case we assume no periodicity.
	const Periodicity periodicbound = simparams() ? simparams()->periodicbound : PERIODIC_NONE;

	// create planes
	if (!(periodicbound & PERIODIC_X)) {
		planes.push_back(addPlane(  1,  0,  0, -min.x));
		planes.push_back(addPlane( -1,  0,  0,  max.x));
	}
	if (!(periodicbound & PERIODIC_Y)) {
		planes.push_back(addPlane(  0,  1,  0, -min.y));
		planes.push_back(addPlane(  0, -1,  0,  max.y));
	}
	if (!(periodicbound & PERIODIC_Z)) {
		planes.push_back(addPlane(  0,  0,  1, -min.z));
		planes.push_back(addPlane(  0,  0, -1,  max.z));
	}

	// set world origin and size
	m_origin = min;
	m_size = max - min;

	return planes;
}

void XProblem::addExtraWorldMargin(const double margin)
{
	if (margin >= 0.0)
		m_extra_world_margin = margin;
	else
		printf("WARNING: tried to add negative world margin! Ignoring\n");
}

// set number of layers for dynamic boundaries. Default is 0, which means: autocompute
void XProblem::setDynamicBoundariesLayers(const uint numLayers)
{
	if (simparams()->boundarytype != DYN_BOUNDARY)
		printf("WARNIG: setting number of layers for dynamic boundaries but not using DYN_BOUNDARY!\n");

	// TODO: use autocomputed instead of 3
	const uint suggestedNumLayers = suggestedDynamicBoundaryLayers();
	if (numLayers > 0 && numLayers < suggestedNumLayers)
		printf("WARNIG: number of layers for dynamic boundaries is low (%u), suggested number is %u\n",
			numLayers, suggestedNumLayers);

	m_numDynBoundLayers = numLayers;
}

int XProblem::fill_parts()
{
	// if for debug reason we need to test the position and verse of a plane, we can ask ODE to
	// compute the distance of a probe point from a plane (positive if penetrated, negative out)
	// TODO FIXME chronomerge
	/*
	double3 probe_point = make_double3 (0.5, 0.5, 0.5);
	printf("Test: probe point is distant %g from the bottom plane.\n,
		dGeomPlanePointDepth(m_box_planes[0], probe_point.x, probe_point.y, probe_point.z)
	*/

	//uint particleCounter = 0;
	uint bodies_parts_counter = 0;
	uint hdf5file_parts_counter = 0;
	uint xyzfile_parts_counter = 0;

	for (size_t g = 0, num_geoms = m_geometries.size(); g < num_geoms; g++) {
		PointVect* parts_vector = NULL;
		double dx = 0.0;

		// ignore deleted geometries
		if (!m_geometries[g]->enabled) continue;

		// set dx and recipient vector according to geometry type
		switch (m_geometries[g]->type) {
			case GT_FLUID:
				parts_vector = &m_fluidParts;
				dx = m_deltap;
				break;
			case GT_TESTPOINTS:
				parts_vector = &m_testpointParts;
				dx = physparams()->r0;
				break;
			case GT_FLOATING_BODY:
			case GT_MOVING_BODY:
				parts_vector = &(m_geometries[g]->ptr->GetParts());
				dx = physparams()->r0;
				break;
			default:
				parts_vector = &m_boundaryParts;
				dx = physparams()->r0;
		}

		// Now will set the particle and object mass if still unset
		const double DEFAULT_DENSITY = physparams()->rho0[0];
		// Setting particle mass by means of dx and default density only. This leads to same mass
		// everywhere but possibly slightly different densities.
		const double DEFAULT_PARTICLE_MASS = (dx * dx * dx) * physparams()->rho0[0];

		// Set part mass, if not set already.
		if (m_geometries[g]->type != GT_PLANE && !m_geometries[g]->particle_mass_was_set)
			setParticleMassByDensity(g, DEFAULT_DENSITY);
			// TODO: should the following be an option?
			//setParticleMass(g, DEFAULT_PARTICLE_MASS);

		// Set object mass for floating objects, if not set already
		if (m_geometries[g]->type == GT_FLOATING_BODY && !m_geometries[g]->mass_was_set)
			setMassByDensity(g, DEFAULT_DENSITY);

		// prepare for erase operations
		bool del_fluid = (m_geometries[g]->erase_operation == ET_ERASE_FLUID);
		bool del_bound = (m_geometries[g]->erase_operation == ET_ERASE_BOUNDARY);
		if (m_geometries[g]->erase_operation == ET_ERASE_ALL) del_fluid = del_bound = true;

		double unfill_dx = dx; // or, dp also if (r0!=dp)?
		if (!std::isnan(m_geometries[g]->unfill_radius))
			unfill_dx = m_geometries[g]->unfill_radius;
		// erase operations with existent geometries
		if (del_fluid) {
			if (m_geometries[g]->intersection_type == IT_SUBTRACT)
				m_geometries[g]->ptr->Unfill(m_fluidParts, unfill_dx);
			else
				m_geometries[g]->ptr->Intersect(m_fluidParts, unfill_dx);
		}
		if (del_bound) {
			if (m_geometries[g]->intersection_type == IT_SUBTRACT)
				m_geometries[g]->ptr->Unfill(m_boundaryParts, unfill_dx);
			else
				m_geometries[g]->ptr->Intersect(m_boundaryParts, unfill_dx);
		}

		// after making some space, fill
		switch (m_geometries[g]->fill_type) {
			case FT_BORDER:
				if (simparams()->boundarytype == DYN_BOUNDARY)
					m_geometries[g]->ptr->FillIn(*parts_vector, dx, - m_numDynBoundLayers);
				else
					m_geometries[g]->ptr->FillBorder(*parts_vector, dx);
				break;
			case FT_SOLID:
				m_geometries[g]->ptr->Fill(*parts_vector, dx);
				break;
			case FT_SOLID_BORDERLESS:
				printf("WARNING: borderless not yet implemented; not filling\n");
				break;
			// case FT_NOFILL: ;
			// yes, it is legal to have no "default:": ISO/IEC 9899:1999, section 6.8.4.2
		}

		// floating and moving bodies fill in their local point vector; let's increase
		// the dedicated bodies_parts_counter
		if (m_geometries[g]->type == GT_FLOATING_BODY ||
			m_geometries[g]->type == GT_MOVING_BODY) {
			bodies_parts_counter += m_geometries[g]->ptr->GetParts().size();
		}

		// geometries loaded from HDF5file do not undergo filling, but should be counted as well
		if (m_geometries[g]->has_hdf5_file)
			hdf5file_parts_counter += m_geometries[g]->hdf5_reader->getNParts();
		// ditto for XYZ files
		if (m_geometries[g]->has_xyz_file)
			xyzfile_parts_counter += m_geometries[g]->xyz_reader->getNParts();

#if 0
		// dbg: fill horizontal XY planes with particles, only within the world domain
		if (m_geometries[g]->type == GT_PLANE) {
			Plane *plane = (Plane*)(m_geometries[g]->ptr);
			// only XY planes planes
			if (! (plane->getA() == 0 && plane->getB() == 0) )
				continue;
			// fill will print a warning
			// NOTE: since parts are added to m_boundaryParts, setting part mass is probably pointless
			plane->SetPartMass(dx, physparams()->rho0[0]);
			// will round r0 to fit each dimension
			const uint xpn = (uint) trunc(m_size.x / physparams()->r0 + 0.5);
			const uint ypn = (uint) trunc(m_size.y / physparams()->r0 + 0.5);
			// compute Z
			const double z_coord = - plane->getD() / plane->getC();
			// aux vectors
			const Point start = Point(m_origin.x, m_origin.y, z_coord);
			const Vector v1 = Vector(m_size.x / xpn, 0, 0);
			const Vector v2 = Vector(0, m_size.x / ypn, 0);
			// fill
			parts_vector = &m_boundaryParts;
			for (uint xp = 0; xp <= xpn; xp++)
				for (uint yp = 0; yp <= ypn; yp++)
					parts_vector->push_back( Point( start + xp * v1 + yp*v2 ) );
		}
#endif

#if USE_CHRONO == 1
		/* We need to create a Chrono body if
		 * - There is at least one floating body in the simulation, AND
		 * - Body is not a plane (they work on fluids only, we need big boxes in place of planes) AND
		 * - One of the following holds:
		 *   * Body is FLOATING; then handle_collisions is passed to Chrono (handle_dynamics must be true)
		 *   * Body is MOVING and handle_collisions is true (handle_dynamics must be false);
		 *   * Body is FIXED and handle_collisions is true (handle_dynamics must be false).
		 */
		if ( (m_numFloatingBodies) &&
			 (m_geometries[g]->type != GT_PLANE) &&
			 (m_geometries[g]->handle_dynamics || m_geometries[g]->handle_collisions) ) {

			// Overwrite the computed inertia matrix if user set a custom one
			// NOTE: this must be done before body creation!
			// NOTE: this is needed also if only handle_collision holds

			// Use custom inertia only if entirely finite (no partial overwrite)
			const double i11 = m_geometries[g]->custom_inertia[0];
			const double i22 = m_geometries[g]->custom_inertia[1];
			const double i33 = m_geometries[g]->custom_inertia[2];
			if (isfinite(i11) && isfinite(i22) && isfinite(i33))
				m_geometries[g]->ptr->SetInertia(i11, i22, i33);
			else
				// if no custom inertia has been set, call default Object::SetInertia()
				// NOTE: this overwrites default inertia set by Chrono for "easy" bodies
				// (Box, Sphere, Cylinder), but it would be only necessary for non-primitive
				// geometries (e.g. STL meshes)
				m_geometries[g]->ptr->SetInertia(physparams()->r0);

			// Fix the geometry *before the creation of the Chrono body* if not moving nor floating.
			// TODO: check if it holds for moving
			if (m_geometries[g]->type == GT_FIXED_BOUNDARY)
				m_geometries[g]->ptr->SetFixed();
			// NOTE: could use SetNoSpeedNoAcceleration() for MOVING chrono bodies?
			m_geometries[g]->ptr->BodyCreate(m_bodies_physical_system, m_deltap, m_geometries[g]->handle_collisions);

			// recap object info such as bounding box, mass, inertia matrix, etc.
			// NOTE: BodyPrintInformation() would be meaningless on planes (excluded above) but harmless anyway
			m_geometries[g]->ptr->BodyPrintInformation();
		} // if m_numFloatingBodies > 0
#endif

		// tell Problem to add the proper type of body
		if (m_geometries[g]->type == GT_FLOATING_BODY)
			add_moving_body(m_geometries[g]->ptr, MB_FLOATING);
		else
		if (m_geometries[g]->type == GT_MOVING_BODY) {
			if (m_geometries[g]->measure_forces)
				add_moving_body(m_geometries[g]->ptr, MB_FORCES_MOVING);
			else
				add_moving_body(m_geometries[g]->ptr, MB_MOVING);
		}

	} // iterate on geometries

	// call user-set filtering routine, if any
	filterPoints(m_fluidParts, m_boundaryParts);

	return m_fluidParts.size() + m_boundaryParts.size() + m_testpointParts.size() +
		bodies_parts_counter + hdf5file_parts_counter + xyzfile_parts_counter;
}

void XProblem::copy_planes(PlaneList &planes)
{
	if (m_numPlanes == 0) return;
	// look for planes
	uint currPlaneIdx = 0;
	// NOTE: could iterate on planes only with a map plane_index -> gid
	for (uint gid = 0, num_geoms = m_geometries.size(); gid < num_geoms; gid++) {

		// skip deleted
		if (! m_geometries[gid]->enabled) continue;

		// not a plane?
		if (m_geometries[gid]->type != GT_PLANE) continue;

		Plane *plane = (Plane*)(m_geometries[gid]->ptr);

		planes.push_back( implicit_plane( plane->getA(), plane->getB(), plane->getC(), plane->getD() ) );

		currPlaneIdx++;
	}
}

void XProblem::copy_to_array(BufferList &buffers)
{
	float4 *pos = buffers.getData<BUFFER_POS>();
	double4 *globalPos = buffers.getData<BUFFER_POS_GLOBAL>();
	hashKey *hash = buffers.getData<BUFFER_HASH>();
	float4 *vel = buffers.getData<BUFFER_VEL>();
	particleinfo *info = buffers.getData<BUFFER_INFO>();
	vertexinfo *vertices = buffers.getData<BUFFER_VERTICES>();
	float4 *boundelm = buffers.getData<BUFFER_BOUNDELEMENTS>();
	float4 *eulerVel = buffers.getData<BUFFER_EULERVEL>();

	// NOTEs and TODO
	// - SA currently supported only from file. Support runtime generation?
	// - Warn if loaded particle has different type than filled, but only once

	// particles counters, by type
	uint fluid_parts = 0;
	uint boundary_parts = 0;
	uint vertex_parts = 0;
	uint testpoint_parts = 0;
	// count #particles loaded from HDF5 files. Needed also to adjust connectivity afterward
	uint hdf5_loaded_parts = 0;
	// count #particles loaded from XYZ files. Only for information
	uint xyz_loaded_parts = 0;
	// Total number of filled parts, i.e. in GPUSPH array and ready to be uploaded.
	uint tot_parts = 0;
	// The following hold:
	//   total = fluid_parts + boundary_parts + vertex_parts + testpoint_parts
	//   total >= hdf5_loaded_parts + xyz_loaded_parts
	//   total >= object_parts
	// NOTE: particles loaded from HDF5 or XYZ files are counted both in the respective
	// *_loaded_parts counter and in the type counter (e.g. fluid_parts).

	// store mass for each particle type
	double fluid_part_mass = NAN;
	double boundary_part_mass = NAN;
	double vertex_part_mass = NAN;

	// we use a simple map for the HDF5_id->id map translation (connectivity fix)
	map<uint, uint> hdf5idx_to_idx_map;

	// count how many particles will be loaded from file
	for (size_t g = 0, num_geoms = m_geometries.size(); g < num_geoms; g++) {
		if (m_geometries[g]->has_hdf5_file)
			hdf5_loaded_parts += m_geometries[g]->hdf5_reader->getNParts();
		else
		if (m_geometries[g]->has_xyz_file)
			xyz_loaded_parts += m_geometries[g]->xyz_reader->getNParts();
	}

	// copy filled testpoint parts
	// NOTE: filling testpoint parts first so that if they are a fixed number they will have
	// the same particle id, independently from the deltap used
	for (uint i = tot_parts; i < tot_parts + m_testpointParts.size(); i++) {
		info[i] = make_particleinfo(PT_TESTPOINT, 0, i);
		calc_localpos_and_hash(m_testpointParts[i - tot_parts], info[i], pos[i], hash[i]);
		globalPos[i] = m_testpointParts[i - tot_parts].toDouble4();
		// Compute density for hydrostatic filling. FIXME for multifluid
		float rho = physparams()->rho0[0];
		if (m_hydrostaticFilling && simparams()->boundarytype == DYN_BOUNDARY)
			rho = density(m_waterLevel - globalPos[i].z, 0);
		vel[i] = make_float4(0, 0, 0, rho);
		if (eulerVel)
			eulerVel[i] = make_float4(0);
		if (i == tot_parts)
			boundary_part_mass = pos[i].w;
	}
	tot_parts += m_testpointParts.size();
	testpoint_parts += m_testpointParts.size();

	// copy filled fluid parts
	for (uint i = tot_parts; i < tot_parts + m_fluidParts.size(); i++) {
		info[i]= make_particleinfo(PT_FLUID,0,i);
		calc_localpos_and_hash(m_fluidParts[i - tot_parts], info[i], pos[i], hash[i]);
		globalPos[i] = m_fluidParts[i - tot_parts].toDouble4();
		// Compute density for hydrostatic filling. FIXME for multifluid
		float rho = physparams()->rho0[0];
		if (m_hydrostaticFilling)
			rho = density(m_waterLevel - globalPos[i].z, 0);
		vel[i] = make_float4(0, 0, 0, rho);
		if (eulerVel)
			eulerVel[i] = make_float4(0);
		if (i == tot_parts)
			fluid_part_mass = pos[i].w;
	}
	tot_parts += m_fluidParts.size();
	fluid_parts += m_fluidParts.size();

	// copy filled boundary parts
	for (uint i = tot_parts; i < tot_parts + m_boundaryParts.size(); i++) {
		info[i] = make_particleinfo(PT_BOUNDARY, 0, i);
		calc_localpos_and_hash(m_boundaryParts[i - tot_parts], info[i], pos[i], hash[i]);
		globalPos[i] = m_boundaryParts[i - tot_parts].toDouble4();
		// Compute density for hydrostatic filling. FIXME for multifluid
		float rho = physparams()->rho0[0];
		if (m_hydrostaticFilling && simparams()->boundarytype == DYN_BOUNDARY)
			rho = density(m_waterLevel - globalPos[i].z, 0);
		vel[i] = make_float4(0, 0, 0, rho);
		if (eulerVel)
			eulerVel[i] = make_float4(0);
		if (i == tot_parts)
			boundary_part_mass = pos[i].w;
	}
	tot_parts += m_boundaryParts.size();
	boundary_parts += m_boundaryParts.size();

	// We've already counted the objects in initialize(), but now we need incremental counters
	// to compute the correct object_id according to the insertion order and body type.
	// Specifically, object_ids are coherent with the insertion order but floating bodies
	// are assigned first; then forces bodies; and finally moving bodies.
	// Please note how they count one kind of body, not including the previous category, as
	// opposite as the global counters (e.g. m_numForcesBodies includes m_numFloatingBodies,
	// but forces_nonFloating_bodies_incremental does not).
	// (i.e. it counts forces non-floating bodies only)
	// Also see Problem::add_moving_body()
	uint floating_bodies_incremental = 0;
	uint forces_nonFloating_bodies_incremental = 0;
	uint moving_nonForces_bodies_incremental = 0;
	// finally, a counter of all forces bodies (either floating or not), only for printing information
	uint forces_bodies_incremental = 0;
	// Open boundaries are orthogonal to any kind of bodies, so their object_id will be simply
	// equal to the incremental counter.
	uint open_boundaries_counter = 0;
	// the number of all the particles of rigid bodies will be used to set s_hRbLastIndex
	uint bodies_particles_counter = 0;
	// store particle mass of last added rigid body
	double rigid_body_part_mass = NAN;

	// Filling s_hRbLastIndex[i] requires the knowledge of the number of particles filled by any
	// object_id < i. Unfortunately, since object_id does not follow the GeometryID, we do not have
	// this knowledge. Thus, we prepare a small array we will fill the number of particles per
	// object, and we will use it at the end of the filling process to fill s_hRbLastIndex[] (and
	// fix s_hRbFirstIndex - see comments later).
	uint *body_particle_counters = new uint[m_numForcesBodies];

	// Until now we copied fluid and boundary particles not belonging to floating objects and/or not to be loaded
	// from HDF5 files. Now we iterate on the geometries with the aim to
	// - copy HDF5 particles from HDF5 files (they are not copied to the global particle vectors, and that's the reason
	//   why currently no erase operations are supported with HDF5-loaded geometries);
	// - copy particles of floating objects, since they fill their own point vector;
	// - setup stuff related to floating objects (e.g. object particle count, flags, etc.).
	for (size_t g = 0, num_geoms = m_geometries.size(); g < num_geoms; g++) {

		// planes do not fill particles nor they load from files
		if (m_geometries[g]->type == GT_PLANE)
			continue;

		// skip deleted geometries
		if (!m_geometries[g]->enabled)
			continue;

		// number of particles loaded or filled by the current geometry
		uint current_geometry_particles = 0;
		// id of first boundary particle (both LJ and SA) and number of them in
		// current geometry, used to compute the rb index offset for forces bodies
		uint current_geometry_num_boundary_parts = 0;
		uint current_geometry_first_boundary_id = UINT_MAX;

		// object id (GPUSPH, not Chrono) that will be used in particleinfo
		// TODO: will also be fluid_number for multifluid
		// NOTE: see comments in the declaration of the counters, above
		uint object_id = 0;
		if (m_geometries[g]->type == GT_FLOATING_BODY)
			object_id = floating_bodies_incremental++;
		else
		if (m_geometries[g]->type == GT_MOVING_BODY && m_geometries[g]->measure_forces)
			// forces body; not floating. ID after floating bodies
			object_id = m_numFloatingBodies + forces_nonFloating_bodies_incremental++;
		else
		if (m_geometries[g]->type == GT_MOVING_BODY)
			// moving body; not floating, no feedback. ID after forces (incl. floating) bodies
			object_id = m_numForcesBodies + moving_nonForces_bodies_incremental++;
		if (m_geometries[g]->type == GT_OPENBOUNDARY)
			// open boundary; nothing to do with bodies
			object_id = open_boundaries_counter++;
		// now update the forces bodies counter, which includes floating ones, only for printing info later
		if (m_geometries[g]->measure_forces)
			forces_bodies_incremental++;

		// load from HDF5 file, whether fluid, boundary, floating or else
		if (m_geometries[g]->has_hdf5_file) {
			// read number of particles
			current_geometry_particles = m_geometries[g]->hdf5_reader->getNParts();
			// utility pointer
			const ReadParticles *hdf5Buffer = m_geometries[g]->hdf5_reader->buf;
			// add every particle
			for (uint i = tot_parts; i < tot_parts + current_geometry_particles; i++) {

				// "i" is the particle index in GPUSPH host arrays, "bi" the one in current HDF5 file)
				const uint bi = i - tot_parts;

				// TODO: define an invalid/unknown particle type?
				// NOTE: update particle counters here, since current_geometry_particles does not distinguish vertex/bound;
				// tot_parts instead is updated in the outer loop
				ushort ptype = PT_FLUID;
				switch (hdf5Buffer[bi].ParticleType) {
					case CRIXUS_FLUID:
						// TODO: warn user if (m_geometries[g]->type != GT_FLUID)
						ptype = PT_FLUID;
						fluid_parts++;
						break;
					case CRIXUS_VERTEX:
						// TODO: warn user if (m_geometries[g]->type == GT_FLUID)
						ptype = PT_VERTEX;
						vertex_parts++;
						break;
					case CRIXUS_BOUNDARY:
						// TODO: warn user if (m_geometries[g]->type == GT_FLUID)
						ptype = PT_BOUNDARY;
						boundary_parts++;
						break;
					default:
						// TODO: print warning or throw fatal
						break;
				}

				// compute particle info, local pos, cellhash
				// NOTE: using explicit constructor make_particleinfo_by_ids() since some flags may
				// be set afterward (e.g. in initializeParticles() callback)
				info[i] = make_particleinfo_by_ids(ptype, 0, object_id, i);

				// set appropriate particle flags
				switch (m_geometries[g]->type) {
					case GT_MOVING_BODY:
						SET_FLAG(info[i], FG_MOVING_BOUNDARY);
						if (m_geometries[g]->measure_forces)
							SET_FLAG(info[i], FG_COMPUTE_FORCE);
						break;
					case GT_FLOATING_BODY:
						SET_FLAG(info[i], FG_MOVING_BOUNDARY | FG_COMPUTE_FORCE);
						break;
					case GT_OPENBOUNDARY:
						const ushort VELOCITY_DRIVEN_FLAG =
							(m_geometries[g]->velocity_driven ? FG_VELOCITY_DRIVEN : 0);
						SET_FLAG(info[i], FG_INLET | FG_OUTLET | VELOCITY_DRIVEN_FLAG);
						break;
				}

				Point tmppoint = Point(hdf5Buffer[bi].Coords_0, hdf5Buffer[bi].Coords_1, hdf5Buffer[bi].Coords_2,
					physparams()->rho0[0]*hdf5Buffer[bi].Volume);
				calc_localpos_and_hash(tmppoint, info[i], pos[i], hash[i]);
				globalPos[i] = tmppoint.toDouble4();

				// Compute density for hydrostatic filling. FIXME for multifluid
				float rho = physparams()->rho0[0];
				if (m_hydrostaticFilling && (ptype == PT_FLUID || simparams()->boundarytype == DYN_BOUNDARY))
					rho = density(m_waterLevel - globalPos[i].z, 0);
				vel[i] = make_float4(0, 0, 0, rho);

				// Update boundary particles counters for rb indices
				// NOTE: the same check will be done for non-HDF5 bodies
				if (ptype == PT_BOUNDARY && COMPUTE_FORCE(info[i])) {
					current_geometry_num_boundary_parts++;
					if (current_geometry_first_boundary_id == UINT_MAX)
						current_geometry_first_boundary_id = id(info[i]); // which should be == i
				}

				if (eulerVel)
					eulerVel[i] = make_float4(0);

				// store particle mass for current type, if it was not store already
				if (ptype == PT_FLUID && !isfinite(fluid_part_mass))
					fluid_part_mass = pos[i].w;
				else
				if (ptype == PT_BOUNDARY && !isfinite(boundary_part_mass))
					boundary_part_mass = pos[i].w;
				else
				if (ptype == PT_VERTEX && !isfinite(vertex_part_mass))
					vertex_part_mass = pos[i].w;
				// also set rigid_body_part_mass, which is orthogonal the the previous values
				// TODO: with SA bounds, this value has little meaning or should be split
				if ((m_geometries[g]->type == GT_FLOATING_BODY ||
					 m_geometries[g]->type == GT_MOVING_BODY) &&
					 !isfinite(rigid_body_part_mass))
					rigid_body_part_mass = pos[i].w;

				// load boundary-specific data (SA bounds only)
				if (ptype == PT_BOUNDARY) {
					if (m_geometries[g]->flip_normals) {
						// NOTE: simulating with flipped normals has not been numerically validated...
						// invert the order of vertices so that for the mass it is m_ref - m_v
						vertices[i].x = hdf5Buffer[bi].VertexParticle3;
						vertices[i].y = hdf5Buffer[bi].VertexParticle2;
						vertices[i].z = hdf5Buffer[bi].VertexParticle1;
						// load with inverted sign
						boundelm[i].x = - hdf5Buffer[bi].Normal_0;
						boundelm[i].y = - hdf5Buffer[bi].Normal_1;
						boundelm[i].z = - hdf5Buffer[bi].Normal_2;
					} else {
						// regular loading
						vertices[i].x = hdf5Buffer[bi].VertexParticle1;
						vertices[i].y = hdf5Buffer[bi].VertexParticle2;
						vertices[i].z = hdf5Buffer[bi].VertexParticle3;
						boundelm[i].x = hdf5Buffer[bi].Normal_0;
						boundelm[i].y = hdf5Buffer[bi].Normal_1;
						boundelm[i].z = hdf5Buffer[bi].Normal_2;
					}

					boundelm[i].w = hdf5Buffer[bi].Surface;
				}

				// update hash map
				if (ptype == PT_VERTEX)
					hdf5idx_to_idx_map[ hdf5Buffer[bi].AbsoluteIndex ] = i;

			} // for every particle in the HDF5 buffer

		} else // if (m_geometries[g]->has_hdf5_file)
		// load from HDF5 file, whether fluid, boundary, floating or else
		if (m_geometries[g]->has_xyz_file) {

			// read number of particles
			current_geometry_particles = m_geometries[g]->xyz_reader->getNParts();

			// all particles in XYZ file will have the same type and flags
			ushort ptype = PT_FLUID;
			flag_t pflags = 0;

			// update particle counters, set ptype, flags - all same for the whole XYZ geometry
			switch (m_geometries[g]->type) {
				case GT_FLUID:
					ptype = PT_FLUID;
					fluid_parts += current_geometry_particles;
					break;
				case GT_FIXED_BOUNDARY:
					ptype = PT_BOUNDARY;
					boundary_parts += current_geometry_particles;
					break;
				case GT_TESTPOINTS:
					ptype = PT_TESTPOINT;
					testpoint_parts += current_geometry_particles;
					break;
				case GT_MOVING_BODY:
					pflags = FG_MOVING_BOUNDARY;
					if (m_geometries[g]->measure_forces)
						pflags |= FG_COMPUTE_FORCE;
					ptype = PT_BOUNDARY;
					boundary_parts += current_geometry_particles;
					break;
				case GT_FLOATING_BODY:
					pflags = FG_MOVING_BOUNDARY | FG_COMPUTE_FORCE;
					ptype = PT_BOUNDARY;
					boundary_parts += current_geometry_particles;
					break;
				case GT_OPENBOUNDARY:
					const ushort VELOCITY_DRIVEN_FLAG =
						(m_geometries[g]->velocity_driven ? FG_VELOCITY_DRIVEN : 0);
					pflags = FG_INLET | FG_OUTLET | VELOCITY_DRIVEN_FLAG;
					// TODO FIXME: check compatibility with new non-SA inlets
					ptype = PT_BOUNDARY;
					boundary_parts += current_geometry_particles;
					break;
				}
			// TODO: nothing else is possible since this is checked while adding the
			// geometry, should we double-check again? And a default

			// utility pointer
			const PointVect *xyzBuffer = &(m_geometries[g]->xyz_reader->points);

			// add every particle
			for (uint i = tot_parts; i < tot_parts + current_geometry_particles; i++) {

				// "i" is the particle index in GPUSPH host arrays, "bi" the one in current XZY file)
				const uint bi = i - tot_parts;

				// compute particle info, local pos, cellhash
				// NOTE: using explicit constructor make_particleinfo_by_ids() since some flags may
				// be set afterward (e.g. in initializeParticles() callback)
				info[i] = make_particleinfo_by_ids(ptype, 0, object_id, i);

				// set appropriate particle flags
				SET_FLAG(info[i], pflags);

				// NOTE: reading the mass from the object, even if it is an empty STL
				Point tmppoint = Point((*xyzBuffer)[bi](0), (*xyzBuffer)[bi](1), (*xyzBuffer)[bi](2),
					m_geometries[g]->ptr->GetPartMass());
				calc_localpos_and_hash(tmppoint, info[i], pos[i], hash[i]);
				globalPos[i] = tmppoint.toDouble4();

				// Compute density for hydrostatic filling. FIXME for multifluid
				float rho = physparams()->rho0[0];
				if (m_hydrostaticFilling && (ptype == PT_FLUID || simparams()->boundarytype == DYN_BOUNDARY))
					rho = density(m_waterLevel - globalPos[i].z, 0);
				vel[i] = make_float4(0, 0, 0, rho);

				// Update boundary particles counters for rb indices
				// NOTE: the same check will be done for non-HDF5 bodies
				if (ptype == PT_BOUNDARY && m_geometries[g]->measure_forces) {
					current_geometry_num_boundary_parts++;
					if (current_geometry_first_boundary_id == UINT_MAX)
						current_geometry_first_boundary_id = id(info[i]); // which should be == i
				}

				if (eulerVel)
					eulerVel[i] = make_float4(0);

				// store particle mass for current type, if it was not store already
				if (ptype == PT_FLUID && !isfinite(fluid_part_mass))
					fluid_part_mass = pos[i].w;
				else
				if (ptype == PT_BOUNDARY && !isfinite(boundary_part_mass))
					boundary_part_mass = pos[i].w;
				// no else supported yet

				// also set rigid_body_part_mass, which is orthogonal the the previous values
				if ((m_geometries[g]->type == GT_FLOATING_BODY ||
					 m_geometries[g]->type == GT_MOVING_BODY) &&
					 !isfinite(rigid_body_part_mass))
					rigid_body_part_mass = pos[i].w;

			} // for every particle in the XYZ buffer

		} // if (m_geometries[g]->has_xyz_file)

		// copy particles from the point vector of objects which have not been loaded from file
		if ( (m_geometries[g]->type == GT_FLOATING_BODY || m_geometries[g]->type == GT_MOVING_BODY)
				&& !(m_geometries[g]->has_hdf5_file) && !(m_geometries[g]->has_xyz_file)) {
			// not loading from file: take object vector
			PointVect & rbparts = m_geometries[g]->ptr->GetParts();
			current_geometry_particles = rbparts.size();
			// copy particles
			for (uint i = tot_parts; i < tot_parts + current_geometry_particles; i++) {
				// TODO FIXME MERGE
				// NOTE: using explicit constructor make_particleinfo_by_ids() since some flags may
				// be set afterward (e.g. in initializeParticles() callback)
				info[i] = make_particleinfo_by_ids(PT_BOUNDARY, 0, object_id, i);
				calc_localpos_and_hash(rbparts[i - tot_parts], info[i], pos[i], hash[i]);
				globalPos[i] = rbparts[i - tot_parts].toDouble4();
				// Compute density for hydrostatic filling. FIXME for multifluid
				float rho = physparams()->rho0[0];
				if (m_hydrostaticFilling && simparams()->boundarytype == DYN_BOUNDARY)
					rho = density(m_waterLevel - globalPos[i].z, 0);
				vel[i] = make_float4(0, 0, 0, rho);
				if (eulerVel)
					// there should be no eulerVel with LJ bounds, but it is safe to init the array anyway
					eulerVel[i] = make_float4(0);
				// NOTE: setting/showing rigid_body_part_mass only makes sense with non-SA bounds
				if (m_geometries[g]->type == GT_FLOATING_BODY && !isfinite(rigid_body_part_mass))
					rigid_body_part_mass = pos[i].w;
				// set appropriate particle flags
				switch (m_geometries[g]->type) {
					case GT_MOVING_BODY:
						SET_FLAG(info[i], FG_MOVING_BOUNDARY);
						if (m_geometries[g]->measure_forces)
							SET_FLAG(info[i], FG_COMPUTE_FORCE);
						break;
					case GT_FLOATING_BODY:
						SET_FLAG(info[i], FG_MOVING_BOUNDARY | FG_COMPUTE_FORCE);
						break;
					// not possible
					//case GT_OPENBOUNDARY:
					//	break;
				}

				// Update boundary particles counters for rb indices
				// NOTE: this is the safest way to update the counters, although
				// with LJ boundaries we could directly set the values afterwards
				// instead of checking every particle
				if (COMPUTE_FORCE(info[i])) {
					current_geometry_num_boundary_parts++;
					if (current_geometry_first_boundary_id == UINT_MAX)
						current_geometry_first_boundary_id = id(info[i]); // which should be == i
				}

			} // for every particle of body
		} // if current geometry is a body and is not loaded from file

		// settings related to objects for which we compute the forces, regardless they were loaded from file or not
		if (m_geometries[g]->measure_forces) {
			// In s_hRbFirstIndex it is stored the id of the first particle of current body (changed
			// in sign, since it is used as an offset) plus the number of previously filled object
			// particles. The former addendum is set here, the latter will be added later (when we'll
			// know the number of particles of all the bodies).
			gdata->s_hRbFirstIndex[object_id] = - (int)current_geometry_first_boundary_id;

			// update counter of rigid body particles
			body_particle_counters[object_id] = current_geometry_num_boundary_parts;

			// recap on stdout
			cout << "Rigid body " << forces_bodies_incremental << ": " << current_geometry_particles <<
				" parts, mass " << rigid_body_part_mass << ", object mass " << m_geometries[g]->ptr->GetMass() << "\n";

			// reset value to spot possible anomalies in next bodies
			rigid_body_part_mass = NAN;
		}

		// update object num parts
		if (m_geometries[g]->type == GT_FLOATING_BODY ||
			m_geometries[g]->type == GT_MOVING_BODY) {
			// set numParts, which will be read while allocating device buffers for obj parts
			// NOTE: this is strictly necessary only for hdf5-loaded objects, because
			// when numparts==0, Object uses rbparts.size().
			m_geometries[g]->ptr->SetNumParts(current_geometry_num_boundary_parts);
		}

		// update global particle counter
		tot_parts += current_geometry_particles;
	} // for each geometry

	// Now we fix s_hRbFirstIndex and fill s_hRbLastIndex. We iterate on all the
	// forces bodies by means of their object id, which is basically the insertion
	// order after being sorted by body type, to keep and incremental particle counter.
	uint incremental_bodies_part_counter = 0;
	for (uint obj_id = 0; obj_id < m_numForcesBodies; obj_id++) {
		// s_hRbFirstIndex is currently -first_bound_id; shift it further according to the previous bodies
		gdata->s_hRbFirstIndex[obj_id] += incremental_bodies_part_counter;
		// now let's increment incremental_bodies_part_counter with current body
		incremental_bodies_part_counter += body_particle_counters[obj_id];
		// memo: s_hRbLastIndex, as used in the reduction, is inclusive (thus -1)
		gdata->s_hRbLastIndex[obj_id] = incremental_bodies_part_counter - 1;
#if 0 // DBG info
		printf(" DBG: s_hRbFirstIndex[%u] = %d, s_hRbLastIndex[%u] = %u\n",
			obj_id, gdata->s_hRbFirstIndex[obj_id], obj_id, gdata->s_hRbLastIndex[obj_id]);
#endif
	}
	delete [] body_particle_counters;

	// fix connectivity by replacing Crixus' AbsoluteIndex with local index
	// TODO: instead of iterating on all the particles, we could create a list of boundary particles while
	// loading them from file, and here iterate only on that vector
	if (simparams()->boundarytype == SA_BOUNDARY && hdf5_loaded_parts > 0) {
		cout << "Fixing connectivity..." << flush;
		for (uint i=0; i< tot_parts; i++)
			if (BOUNDARY(info[i])) {
				if (hdf5idx_to_idx_map.count(vertices[i].x) == 0 ||
					hdf5idx_to_idx_map.count(vertices[i].y) == 0 ||
					hdf5idx_to_idx_map.count(vertices[i].z) == 0 ) {
					printf("FATAL: connectivity: particle id %u index %u loaded from HDF5 points to non-existing vertices (%u,%u,%u)!\n",
						id(info[i]), i, vertices[i].x, vertices[i].y, vertices[i].z );
					exit(1);
				} else {
					vertices[i].x = id(info[ hdf5idx_to_idx_map.find(vertices[i].x)->second ]);
					vertices[i].y = id(info[ hdf5idx_to_idx_map.find(vertices[i].y)->second ]);
					vertices[i].z = id(info[ hdf5idx_to_idx_map.find(vertices[i].z)->second ]);
				}
			}
		cout << "DONE" << "\n";
		hdf5idx_to_idx_map.clear();
	}

	// FIXME: move this somewhere else
	printf("Open boundaries: %zu\n", m_numOpenBoundaries);

	cout << "Fluid: " << fluid_parts << " parts, mass " << fluid_part_mass << "\n";
	cout << "Boundary: " << boundary_parts << " parts, mass " << boundary_part_mass << "\n";
	if (simparams()->boundarytype == SA_BOUNDARY)
		cout << "Vertices: " << vertex_parts << " parts, mass " << vertex_part_mass << "\n";
	cout << "Testpoint: " << testpoint_parts << " parts\n";
	cout << "Tot: " << tot_parts << " particles\n";
	flush(cout);

	// call user-set initialization routine, if any
	initializeParticles(buffers, tot_parts);
}

// callback for filtering out points before they become particles (e.g. unfills/cuts)
void XProblem::filterPoints(PointVect &fluidParts, PointVect &boundaryParts)
{
		// Default: do nothing

	/*
	// Example usage
	// TODO
	*/
}

// callback for initializing particles with custom values
void XProblem::initializeParticles(BufferList &buffers, const uint numParticles)
{
	// Default: do nothing

	/*
	// Example usage

	// 1. warn the user if this is expected to take much time
	printf("Initializing particles velocity...\n");

	// 2. grab the particle arrays from the buffer list
	float4 *vel = buffers.getData<BUFFER_VEL>();
	particleinfo *info = buffers.getData<BUFFER_INFO>();

	// 3. iterate on the particles
	for (uint i = 0; i < numParticles; i++) {
		// 4. optionally grep with custom filters (e.g. type, size, position, etc.)
		if (FLUID(info[i]))
			// 5. set in loco the desired values
			vel[i].x = 0.1;
	}
	*/
}

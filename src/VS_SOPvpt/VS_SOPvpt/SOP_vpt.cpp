/*
*    Copyright 2018, Sergen Eren <sergeneren@gmail.com>.
*
*    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
*
*    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
*
*    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
*       in the documentation and/or other materials provided with the distribution.
*
*    3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
*    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
*    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
*    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
*    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
*    EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* Source repository: https://github.com/sergeneren/SOP_vpt
*/


/*! \file
*
*   SOP level volumetric path tracer implementation header file
*	builds the baseline for SOP_vpt
*
*/


#include "SOP_vpt.h"
#include "Integrator.h"

//Houdini
#include <GU/GU_Detail.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <OP/OP_Director.h>
#include <OP/OP_AutoLockInputs.h>
#include <PRM/PRM_Include.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_Interrupt.h>
#include <SYS/SYS_Math.h>
#include <SOP/SOP_Node.h>
#include <UT/UT_ThreadedAlgorithm.h>
#include <UT/UT_ThreadedIO.h>
#include <GA/GA_PageHandle.h>
#include <GA/GA_IntrinsicMacros.h>
#include <GA/GA_PageIterator.h>
#include <GU/GU_Prim.h>
#include <GU/GU_RayIntersect.h>


//C++



using namespace VPT; 

void newSopOperator(OP_OperatorTable *table) {

	table->addOperator(new OP_Operator (
		"sop_vpt",
		"SOP_VPT",
		SOP_vpt::myConstructor,
		SOP_vpt::myTemplateList,
		2, // min number of inputs 
		5, // max number of inputs
		0)); 
}


// Parameter defaults
static PRM_Name param_names[] = {

	PRM_Name("res", "Camera Resolution"),
	PRM_Name("sizex", "Camera Size x"),
	PRM_Name("spp", "Samples Per Pixel"),
	PRM_Name("depth", "Ray Depth"),
	PRM_Name("vs", "Volume Samples"),
	PRM_Name("color", "Color"),
	PRM_Name("attenuation", "Attenuation"),
	PRM_Name("density", "Density"),
	PRM_Name("sdm", "Shadow Density Multiplier"),
	PRM_Name("max_dist", "Maximum Distance"),
	PRM_Name("steprate", "Volume Step Rate"),

};


PRM_Template SOP_vpt::myTemplateList[] = {

	PRM_Template(PRM_INT_J, 2 , &param_names[0], PRMzeroDefaults),
	PRM_Template(PRM_FLT, 1 , &param_names[1], PRMzeroDefaults),
	PRM_Template(PRM_INT, 1 , &param_names[2], PRMoneDefaults),
	PRM_Template(PRM_INT, 1 , &param_names[3], PRMtwoDefaults),
	PRM_Template(PRM_INT, 1 , &param_names[4], PRMoneDefaults),
	PRM_Template(PRM_RGB, 3 , &param_names[5], PRMoneDefaults),
	PRM_Template(PRM_RGB, 3 , &param_names[6], PRMoneDefaults),
	PRM_Template(PRM_FLT, 1 , &param_names[7], PRMpointOneDefaults),
	PRM_Template(PRM_FLT, 1 , &param_names[8], PRMoneDefaults),
	PRM_Template(PRM_FLT, 1 , &param_names[9], PRM100Defaults),
	PRM_Template(PRM_FLT, 1 , &param_names[10], PRMpointOneDefaults),
	PRM_Template()
};



OP_Node * SOP_vpt::myConstructor(OP_Network *net, const char *name, OP_Operator *op)
{
	return new SOP_vpt(net, name, op);
}

SOP_vpt::SOP_vpt(OP_Network *net, const char *name, OP_Operator *op) : SOP_Node(net, name, op)
{

	mySopFlags.setManagesDataIDs(true); 

}

SOP_vpt::~SOP_vpt() {}

OP_ERROR SOP_vpt::cookMySop(OP_Context & context)
{
	
	OP_AutoLockInputs inputs(this); 
	if (inputs.lock(context) >= UT_ERROR_ABORT) return error(); 
	
	duplicateSource(0, context); 

	addMessage(SOP_MESSAGE, "Path traces a given scene to grid"); 
	
	fpreal t = context.getTime();
	GA_Index index; 
	
	UT_Vector2 res = RES(t); 
	size_t spp = SPP(t);
	size_t depth = DEPTH(t);
	size_t vs = VS(t);
	float sizex = SIZEX(t); 
	UT_Vector3 color = COLOR(t); 
	UT_Vector3 attenuation = ATTN(t); 
	float density = DENSITY(t); 
	float sdm = SDM(t); 
	float maxdist = MAXDIST(t); 
	float steprate = STEPRATE(t); 
	
	//Get inputs
	
	const GU_Detail *cam_gdp	= inputGeo(1);
	const GU_Detail *prim_gdp	= inputGeo(2);
	const GU_Detail *vol_gdp	= inputGeo(3);
	const GU_Detail *light_gdp	= inputGeo(4);

	//Scene Preprocess
	
	
	//Check Cam
	size_t cam_pts = cam_gdp->getNumPoints();
	if (cam_pts != 1) opError(OP_ERR_INVALID_SRC,"Camera input accepts a single point!!!"); 
	
	//Prepare Lights
	GA_Range light_pt_range = light_gdp->getPointRange();
	if (light_pt_range.isEmpty()) return error();
	GA_ROHandleV3 light_pos(light_gdp->getP()); 
	GA_ROHandleV3 light_color(light_gdp->findPointVectorAttrib("Cd"));
	GA_ROHandleI light_type(light_gdp, GA_ATTRIB_POINT, "type");
	
	UT_Array<Light> lights; 
	for (GA_Iterator it(light_pt_range.begin()); !it.atEnd(); ++it) {

		lights.append();
		if(light_pos.isValid()) lights[it.getOffset()].pos = light_pos.get(it.getOffset());
		else lights[it.getOffset()].pos.assign(0,0,0); 

		if(light_color.isValid()) lights[it.getOffset()].color = light_color.get(it.getOffset());
		else lights[it.getOffset()].color.assign(0,0,0); 

		if(light_type.isValid()) lights[it.getOffset()].type = light_type.get(it.getOffset());
		else lights[it.getOffset()].type = 0; 
	}
	

	GA_RWHandleV3 Cd(gdp->addFloatTuple(GA_ATTRIB_PRIMITIVE, "Cd" , 3)); 
	GA_RWHandleV3 P(gdp->addFloatTuple(GA_ATTRIB_PRIMITIVE, "pos", 3));
	
	GA_ROHandleV3 cam_h(cam_gdp, GA_ATTRIB_POINT, "P"); 
	UT_Vector3F cam = cam_h.get(0); 

	GU_RayIntersect *isect = new GU_RayIntersect(prim_gdp);
	
	GA_Primitive *prim;
	
	GA_Offset prim_offset; 
	for (GA_Iterator prim_it(gdp->getPrimitiveRange()); !prim_it.atEnd(); ++prim_it) { 
		prim = gdp->getPrimitive(prim_it.getOffset());
		Integrator *integrator = new Integrator; 
		UT_Vector3F color(0,0,0);
		UT_Vector3 pos = P.get(prim_it.getOffset());
		color = integrator->render(cam ,pos, prim_gdp, isect, &lights);
		Cd.set(*prim_it, color);
	}

	Cd.bumpDataId();
	
	return error();
}



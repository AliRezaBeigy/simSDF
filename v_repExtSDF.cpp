// Copyright 2016 Coppelia Robotics GmbH. All rights reserved. 
// marc@coppeliarobotics.com
// www.coppeliarobotics.com
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// -------------------------------------------------------------------
// Authors:
// Federico Ferri <federico.ferri.it at gmail dot com>
// -------------------------------------------------------------------

#include "v_repExtSDF.h"
#include "debug.h"
#include "SDFDialog.h"
#include "tinyxml2.h"
#include "SDFParser.h"
#include "stubs.h"
#include "UIFunctions.h"
#include "UIProxy.h"
#include "v_repLib.h"
#include "MyMath.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <QThread>
#ifdef _WIN32
    #ifdef QT_COMPIL
        #include <direct.h>
    #else
        #include <shlwapi.h>
        #pragma comment(lib, "Shlwapi.lib")
    #endif
#endif /* _WIN32 */
#if defined (__linux) || defined (__APPLE__)
    #include <unistd.h>
#define _stricmp strcasecmp
#endif /* __linux || __APPLE__ */

#define CONCAT(x, y, z) x y z
#define strConCat(x, y, z)    CONCAT(x, y, z)

#define PLUGIN_NAME "SDF"
#define VREP_COMPATIBILITY 9    // 1 until 20/1/2013 (1 was a very early beta)
                                // 2 until 10/1/2014 (V-REP3.0.5)
                                // 3: new lock
                                // 4: since V-REP 3.1.2
                                // 5: since after V-REP 3.1.3
                                // 6: since V-REP 3.2.2
                                // 7: since V-REP 3.2.2 rev2
                                // 8: since V-REP 3.3.0 (headless mode detect)
                                // 9: since V-REP 3.3.1 (Using stacks to exchange data with scripts)

LIBRARY vrepLib; // the V-REP library that we will dynamically load and bind
SDFDialog *sdfDialog = NULL;
int menuItemHandle = -1;

using namespace tinyxml2;
using std::set;

void setVrepObjectName(int objectHandle, const char* desiredName)
{
    // Objects in V-REP can only contain a-z, A-Z, 0-9, '_' or exaclty one '#' optionally followed by a number
    std::string baseName(desiredName);
    for(int i = 0; i < baseName.size(); i++)
    {
        char n = baseName[i];
        if(((n < 'a') || (n > 'z')) && ((n < 'A') || (n > 'Z')) && ((n < '0') || (n > '9')))
            baseName[i] = '_';
    }
    std::string objName(baseName);
    int suffix=2;
    while(simSetObjectName(objectHandle, objName.c_str())==-1)
        objName = baseName + boost::lexical_cast<std::string>(suffix++);
}

C7Vector getPose(optional<Pose>& pose)
{
    C7Vector v;
    v.setIdentity();
    if(pose)
    {
        Vector &p = pose->position;
        Orientation &o = pose->orientation;
        v.X.set(p.x, p.y, p.z);
        C4Vector roll, pitch, yaw;
        roll.setEulerAngles(C3Vector(o.roll, 0.0f, 0.0f));
        pitch.setEulerAngles(C3Vector(0.0f, o.pitch, 0.0f));
        yaw.setEulerAngles(C3Vector(0.0f, 0.0f, o.yaw));
        v.Q = yaw * pitch * roll;
    }
    return v;
}

C7Vector getPose(Model &model, optional<Pose>& pose)
{
    return getPose(model.pose) * getPose(pose);
}

void importWorld(World &world)
{
    std::cout << "Importing world '" << world.name << "'..." << std::endl;
    std::cout << "ERROR: importing worlds not implemented yet" << std::endl;
}

simInt importGeometry(Geometry &geometry, bool static_, bool respondable, double mass)
{
    simInt handle = -1;

    if(geometry.empty)
    {
        handle = simCreateDummy(0, NULL);
    }
    else if(geometry.box || geometry.sphere || geometry.cylinder)
    {
        // pure shape
        simInt primitiveType =
            geometry.box ? 0 :
            geometry.sphere ? 1 :
            geometry.cylinder ? 2 :
            -1;
        simInt options = 0
            + 1 // backface culling
            + 2 // show edges
            + (respondable ? 8 : 0)
            + (static_ ? 16 : 0) // static shape?
            ;
        simFloat sizes[3];
        if(geometry.box)
        {
            sizes[0] = geometry.box->size.x;
            sizes[1] = geometry.box->size.y;
            sizes[2] = geometry.box->size.z;
        }
        else if(geometry.sphere)
        {
            sizes[0] = sizes[1] = sizes[2] = 2 * geometry.sphere->radius;
        }
        else if(geometry.cylinder)
        {
            sizes[0] = sizes[1] = 2 * geometry.cylinder->radius;
            sizes[2] = geometry.cylinder->length;
        }
        handle = simCreatePureShape(primitiveType, options, sizes, mass, NULL);
    }
    else if(geometry.heightmap)
    {
        simInt options = 0
            + 1 // backface culling
            + 2 // overlay mesh visible
            + (respondable ? 0 : 8)
            ;
        simFloat shadingAngle = 45;
        simInt xPointCount = 0;
        simInt yPointCount = 0;
        simFloat xSize = 0;
        simFloat *heights = 0;
        handle = simCreateHeightfieldShape(options, shadingAngle, xPointCount, yPointCount, xSize, heights);
    }
    else if(geometry.mesh)
    {
        simInt options = 0
            + 1 // backface culling
            + 2 // show edges
            ;
        simFloat shadingAngle = 45;
        simFloat *vertices = 0;
        simInt verticesSize = 0;
        simInt *indices = 0;
        simInt indicesSize = 0;
        handle = simCreateMeshShape(options, shadingAngle, vertices, verticesSize, indices, indicesSize, NULL);
    }
    else if(geometry.image)
    {
        std::cout << "ERROR: image geometry not currently supported" << std::endl;
    }
    else if(geometry.plane)
    {
        std::cout << "ERROR: plane geometry not currently supported" << std::endl;
    }
    else if(geometry.polyline)
    {
        std::cout << "ERROR: polyline geometry not currently supported" << std::endl;
    }

    return handle;
}

void importModelLink(Model &model, Link &link, simInt parentJointHandle)
{
    std::cout << "Importing link '" << link.name << "' of model '" << model.name << "'..." << std::endl;

    C7Vector pose = getPose(model, link.pose);

    double mass = 0;
    if(link.inertial && link.inertial->mass)
    {
        mass = *link.inertial->mass;
    }

    vector<simInt> shapeHandlesColl;
    BOOST_FOREACH(LinkCollision &x, link.collisions)
    {
        simInt shapeHandle = importGeometry(x.geometry, false, true, mass);
        if(shapeHandle == -1) continue;
        shapeHandlesColl.push_back(shapeHandle);
        C7Vector poseColl = x.pose ? getPose(model, x.pose) : pose;
        simSetObjectPosition(shapeHandle, -1, poseColl.X.data);
        simSetObjectOrientation(shapeHandle, -1, poseColl.Q.getEulerAngles().data);
    }
    simInt shapeHandleColl = -1;
    if(shapeHandlesColl.size() == 0)
    {
        BoxGeometry box;
        Geometry g;
        g.box = BoxGeometry();
        g.box->size.x = 0.01;
        g.box->size.y = 0.01;
        g.box->size.z = 0.01;
        shapeHandleColl = importGeometry(g, false, false, mass);
    }
    else if(shapeHandlesColl.size() == 1)
    {
        shapeHandleColl = shapeHandlesColl[0];
    }
    else if(shapeHandlesColl.size() > 1)
    {
        shapeHandleColl = simGroupShapes(&shapeHandlesColl[0], shapeHandlesColl.size());
    }
    link.vrepHandle = shapeHandleColl;
    if(model.vrepHandle == -1)
        model.vrepHandle = link.vrepHandle;
    std::stringstream ss;
    ss << link.name << "_" << "collision";
    setVrepObjectName(shapeHandleColl, ss.str().c_str());

    if(link.inertial && link.inertial->inertia)
    {
        InertiaMatrix &i = *link.inertial->inertia;
        float inertia[9] = {
            i.ixx, i.ixy, i.ixz,
            i.ixy, i.iyy, i.iyz,
            i.ixz, i.iyz, i.izz
        };
        C4X4Matrix t(getPose(link.inertial->pose).getMatrix());
        float m[12] = {
            t.M(0,0), t.M(0,1), t.M(0,2), t.X(0),
            t.M(1,0), t.M(1,1), t.M(1,2), t.X(1),
            t.M(2,0), t.M(2,1), t.M(2,2), t.X(2)
        };
        simSetShapeMassAndInertia(shapeHandleColl, mass, inertia, C3Vector::zeroVector.data, m);
    }

    if(parentJointHandle != -1)
    {
        //simSetObjectParent(shapeHandleColl, parentJointHandle, true);
    }

    if(true /* hideCollisions */)
    {
        simSetObjectIntParameter(shapeHandleColl, sim_objintparam_visibility_layer, 256); // assign collision to layer 9
    }

    BOOST_FOREACH(LinkVisual &x, link.visuals)
    {
        simInt shapeHandle = importGeometry(x.geometry, true, false, 0);
        if(shapeHandle == -1) continue;
        C7Vector poseVis = x.pose ? getPose(model, x.pose) : pose;
        simSetObjectPosition(shapeHandle, -1, poseVis.X.data);
        simSetObjectOrientation(shapeHandle, -1, poseVis.Q.getEulerAngles().data);
        simSetObjectParent(shapeHandle, shapeHandleColl, true);
        std::stringstream ss;
        ss << link.name << "_" << x.name;
        setVrepObjectName(shapeHandle, ss.str().c_str());
    }
}

simInt importModelJoint(Model &model, Joint &joint, simInt parentLinkHandle)
{
    std::cout << "Importing joint '" << joint.name << "' of model '" << model.name << "'..." << std::endl;

    simInt handle = -1;

    if(!joint.axis || joint.axis2)
    {
        std::cout << "ERROR: joint must have exactly one axis" << std::endl;
        return handle;
    }

    const Axis &axis = *joint.axis;

    if(joint.type == "revolute" || joint.type == "prismatic")
    {
        simInt subType =
            joint.type == "revolute" ? sim_joint_revolute_subtype :
            joint.type == "prismatic" ? sim_joint_prismatic_subtype :
            -1;
        handle = simCreateJoint(subType, sim_jointmode_force, 2, NULL, NULL, NULL);

        if(axis.limit)
        {
            const AxisLimits &limits = *axis.limit;

            float interval[2] = {limits.lower, limits.upper - limits.lower};
            simSetJointInterval(handle, 0, interval);

            if(limits.effort)
            {
                simSetJointForce(handle, *limits.effort);
            }

            if(limits.velocity)
            {
                simSetObjectFloatParameter(handle, sim_jointfloatparam_upper_limit, *limits.velocity);
            }
        }

        if(false /* positionCtrl */)
        {
            simSetObjectIntParameter(handle, sim_jointintparam_motor_enabled, 1);
        }

        if(true /* hideJoints */)
        {
            simSetObjectIntParameter(handle, sim_objintparam_visibility_layer, 512); // layer 10
        }
    }
    else if(joint.type == "ball")
    {
        handle = simCreateJoint(sim_joint_spherical_subtype, sim_jointmode_force, 2, NULL, NULL, NULL);
    }
    else if(joint.type == "fixed")
    {
        int intParams[5]={1,4,4,0,0};
        float floatParams[5]={0.02f,1.0f,1.0f,0.0f,0.0f};
        handle = simCreateForceSensor(0, intParams, floatParams, NULL);
    }
    else
    {
        std::cout << "Joint type '" << joint.type << "' is not supported" << std::endl;
    }

    if(handle == -1)
        return handle;

    joint.vrepHandle = handle;

    if(parentLinkHandle != -1)
    {
        //simSetObjectParent(handle, parentLinkHandle, true);
    }

    setVrepObjectName(handle, joint.name.c_str());

    return handle;
}

void adjustJointPose(Model &model, Joint *joint, simInt childLinkHandle)
{
    const Axis &axis = *joint->axis;

    // compute joint axis orientation:
    C4X4Matrix jointAxisMatrix;
    jointAxisMatrix.setIdentity();
    C3Vector axisVec(axis.xyz.x, axis.xyz.y, axis.xyz.z);
    C3Vector rotAxis;
    float rotAngle=0.0f;
    if(axisVec(2) < 1.0f)
    {
        if(axisVec(2) <= -1.0f)
            rotAngle = 3.14159265359f;
        else
            rotAngle = acosf(axisVec(2));
        rotAxis(0) = -axisVec(1);
        rotAxis(1) = axisVec(0);
        rotAxis(2) = 0.0f;
        rotAxis.normalize();
        C7Vector m(jointAxisMatrix);
        float alpha = -atan2(rotAxis(1), rotAxis(0));
        float beta = atan2(-sqrt(rotAxis(0) * rotAxis(0) + rotAxis(1) * rotAxis(1)), rotAxis(2));
        C7Vector r;
        r.X.clear();
        r.Q.setEulerAngles(0.0f, 0.0f, alpha);
        m = r * m;
        r.Q.setEulerAngles(0.0f, beta, 0.0f);
        m = r * m;
        r.Q.setEulerAngles(0.0f, 0.0f, rotAngle);
        m = r * m;
        r.Q.setEulerAngles(0.0f, -beta, 0.0f);
        m = r * m;
        r.Q.setEulerAngles(0.0f, 0.0f, -alpha);
        m = r * m;
        jointAxisMatrix = m.getMatrix();
    }

    C4X4Matrix jointMatrix;
    if(axis.useParentModelFrame)
    {
        jointMatrix = getPose(model, joint->pose).getMatrix();
        jointMatrix = jointMatrix * jointAxisMatrix;
    }
    else
    {
        simGetObjectPosition(childLinkHandle, -1, jointMatrix.X.data);
        C3Vector euler;
        simGetObjectOrientation(childLinkHandle, -1, euler.data);
        jointMatrix.M.setEulerAngles(euler);
        jointMatrix = jointMatrix * getPose(joint->pose).getMatrix();
    }
    C7Vector t = jointMatrix.getTransformation();
    simSetObjectPosition(joint->vrepHandle, -1, t.X.data);
    simSetObjectOrientation(joint->vrepHandle, -1, t.Q.getEulerAngles().data);
}

void visitLink(Model &model, Link *link)
{
    set<Joint*> childJoints = link->getChildJoints(model);
    BOOST_FOREACH(Joint *joint, childJoints)
    {
        Link *childLink = joint->getChildLink(model);
        importModelJoint(model, *joint, link->vrepHandle);
        importModelLink(model, *childLink, joint->vrepHandle);
        adjustJointPose(model, joint, childLink->vrepHandle);
        simSetObjectParent(joint->vrepHandle, link->vrepHandle, true);
        simSetObjectParent(childLink->vrepHandle, joint->vrepHandle, true);
        visitLink(model, childLink);
    }
}

void importModel(Model &model)
{
    std::cout << "Importing model '" << model.name << "'..." << std::endl;

    bool static_ = true;
    if(model.static_ && *model.static_ == false)
        static_ = false;

    // import model's links starting from top-level links (i.e. those without parent link)
    BOOST_FOREACH(Link &link, model.links)
    {
        if(link.getParentJoint(model)) continue;
        importModelLink(model, link, -1);
        visitLink(model, &link);
    }

    BOOST_FOREACH(Model &x, model.submodels)
    {
        importModel(x);
    }
}

void importActor(Actor &actor)
{
    std::cout << "Importing actor '" << actor.name << "'..." << std::endl;
    std::cout << "ERROR: actors are not currently supported" << std::endl;
}

void importLight(Light &light)
{
    std::cout << "Importing light '" << light.name << "'..." << std::endl;
    std::cout << "ERROR: importing lights not currently supported" << std::endl;
}

void importSDF(SDF &sdf)
{
    std::cout << "Importing SDF file (version " << sdf.version << ")..." << std::endl;
    sdf.dump();
    BOOST_FOREACH(World &x, sdf.worlds)
    {
        importWorld(x);
    }
    BOOST_FOREACH(Model &x, sdf.models)
    {
        importModel(x);
    }
    BOOST_FOREACH(Actor &x, sdf.actors)
    {
        importActor(x);
    }
    BOOST_FOREACH(Light &x, sdf.lights)
    {
        importLight(x);
    }
}

XMLElement *loadAndParseXML(std::string fileName, XMLDocument *pdoc)
{
    XMLError err = pdoc->LoadFile(fileName.c_str());
    if(err != XML_NO_ERROR)
        throw std::string("xml load error");
    XMLElement *root = pdoc->FirstChildElement();
    if(!root)
        throw std::string("xml internal error: cannot get root element");
    return root;
}

void import(SScriptCallBack *p, const char *cmd, import_in *in, import_out *out)
{
    XMLDocument xmldoc;
    XMLElement *root = loadAndParseXML(in->fileName, &xmldoc);
    SDF sdf;
    sdf.parse(root);
    std::cout << "parsed SDF successfully" << std::endl;
    importSDF(sdf);
}

void dump(SScriptCallBack *p, const char *cmd, dump_in *in, dump_out *out)
{
    XMLDocument xmldoc;
    XMLElement *root = loadAndParseXML(in->fileName, &xmldoc);
    SDF sdf;
    sdf.parse(root);
    std::cout << "parsed SDF successfully" << std::endl;
    sdf.dump();
}

VREP_DLLEXPORT unsigned char v_repStart(void* reservedPointer, int reservedInt)
{
    char curDirAndFile[1024];
#ifdef _WIN32
    #ifdef QT_COMPIL
        _getcwd(curDirAndFile, sizeof(curDirAndFile));
    #else
        GetModuleFileName(NULL, curDirAndFile, 1023);
        PathRemoveFileSpec(curDirAndFile);
    #endif
#elif defined (__linux) || defined (__APPLE__)
    getcwd(curDirAndFile, sizeof(curDirAndFile));
#endif

    std::string currentDirAndPath(curDirAndFile);
    std::string temp(currentDirAndPath);
#ifdef _WIN32
    temp+="\\v_rep.dll";
#elif defined (__linux)
    temp+="/libv_rep.so";
#elif defined (__APPLE__)
    temp+="/libv_rep.dylib";
#endif /* __linux || __APPLE__ */
    vrepLib = loadVrepLibrary(temp.c_str());
    if(vrepLib == NULL)
    {
        std::cout << "Error, could not find or correctly load the V-REP library. Cannot start '" PLUGIN_NAME "' plugin.\n";
        return(0);
    }
    if(getVrepProcAddresses(vrepLib)==0)
    {
        std::cout << "Error, could not find all required functions in the V-REP library. Cannot start '" PLUGIN_NAME "' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0);
    }

    int vrepVer;
    simGetIntegerParameter(sim_intparam_program_version, &vrepVer);
    if(vrepVer < 30301) // if V-REP version is smaller than 3.03.01
    {
        std::cout << "Sorry, your V-REP copy is somewhat old. Cannot start '" PLUGIN_NAME "' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0);
    }

    if(simGetBooleanParameter(sim_boolparam_headless) > 0)
    {
        //std::cout << "V-REP runs in headless mode. Cannot start 'Urdf' plugin.\n";
        //unloadVrepLibrary(vrepLib);
        //return(0); // Means error, V-REP will unload this plugin
    }
    else
    {
        QWidget *mainWindow = (QWidget *)simGetMainWindow(1);
        sdfDialog = new SDFDialog(mainWindow);
        simAddModuleMenuEntry("", 1, &menuItemHandle);
        simSetModuleMenuItemState(menuItemHandle, 1, "SDF import...");
    }

    if(!registerScriptStuff())
    {
        std::cout << "Initialization failed.\n";
        unloadVrepLibrary(vrepLib);
        return(0);
    }

    UIProxy::getInstance(); // construct UIProxy here (UI thread)

    return VREP_COMPATIBILITY; // initialization went fine, we return the V-REP compatibility version
}

VREP_DLLEXPORT void v_repEnd()
{
    if(sdfDialog)
        delete sdfDialog;

    UIFunctions::destroyInstance();
    UIProxy::destroyInstance();

    unloadVrepLibrary(vrepLib); // release the library
}

VREP_DLLEXPORT void* v_repMessage(int message, int* auxiliaryData, void* customData, int* replyData)
{
    // Keep following 5 lines at the beginning and unchanged:
    static bool refreshDlgFlag = true;
    int errorModeSaved;
    simGetIntegerParameter(sim_intparam_error_report_mode, &errorModeSaved);
    simSetIntegerParameter(sim_intparam_error_report_mode, sim_api_errormessage_ignore);
    void* retVal=NULL;

    static bool firstInstancePass = true;
    if(firstInstancePass && message == sim_message_eventcallback_instancepass)
    {
        firstInstancePass = false;
        UIFunctions::getInstance(); // construct UIFunctions here (SIM thread)
    }

    if(message == sim_message_eventcallback_simulationended)
    { // Simulation just ended
        // TODO: move this to sim_message_eventcallback_simulationabouttoend
    }

    if(message == sim_message_eventcallback_menuitemselected)
    { // A custom menu bar entry was selected
        if(auxiliaryData[0] == menuItemHandle)
        {
            // 'SDF Import...' was selected
            simChar* pathAndFile = simFileDialog(sim_filedlg_type_load, "SDF PLUGIN LOADER", "", "", "SDF Files", "sdf");
            if(pathAndFile != NULL)
            {
                std::string f(pathAndFile);
                simReleaseBuffer(pathAndFile);
                sdfDialog->showDialogForFile(f);
            }
        }
    }

    // Keep following unchanged:
    simSetIntegerParameter(sim_intparam_error_report_mode, errorModeSaved); // restore previous settings
    return(retVal);
}


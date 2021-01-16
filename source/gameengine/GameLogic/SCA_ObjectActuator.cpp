/*
 * Do translation/rotation actions
 *
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/SCA_ObjectActuator.cpp
 *  \ingroup ketsji
 */

#include "SCA_ObjectActuator.h"

#include "CM_Message.h"
#include "KX_GameObject.h"
#include "KX_PyMath.h"  // For PyVecTo - should this include be put in EXP_PyObjectPlus?
#include "PHY_ICharacter.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_ObjectActuator::SCA_ObjectActuator(SCA_IObject *gameobj,
                                       KX_GameObject *refobj,
                                       const MT_Vector3 &force,
                                       const MT_Vector3 &torque,
                                       const MT_Vector3 &dloc,
                                       const MT_Vector3 &drot,
                                       const MT_Vector3 &linV,
                                       const MT_Vector3 &angV,
                                       const short damping,
                                       const KX_LocalFlags &flag)
    : SCA_IActuator(gameobj, KX_ACT_OBJECT),
      m_force(force),
      m_torque(torque),
      m_dloc(dloc),
      m_drot(drot),
      m_linear_velocity(linV),
      m_angular_velocity(angV),
      m_linear_length2(0.0f),
      m_current_linear_factor(0.0f),
      m_current_angular_factor(0.0f),
      m_damping(damping),
      m_previous_error(0.0f, 0.0f, 0.0f),
      m_error_accumulator(0.0f, 0.0f, 0.0f),
      m_bitLocalFlag(flag),
      m_reference(refobj),
      m_linear_damping_active(false),
      m_angular_damping_active(false),
      m_jumping(false)
{
  if (m_bitLocalFlag.ServoControl) {
    // in servo motion, the force is local if the target velocity is local
    m_bitLocalFlag.Force = m_bitLocalFlag.LinearVelocity;

    m_pid = m_torque;
  }
  if (m_bitLocalFlag.CharacterMotion) {
    KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());
    PHY_ICharacter *character =
        parent->GetScene()->GetPhysicsEnvironment()->GetCharacterController(parent);

    if (!character) {
      CM_LogicBrickWarning(
          this,
          "character motion enabled on non-character object, falling back to simple motion.");
      m_bitLocalFlag.CharacterMotion = false;
    }
  }
  if (m_reference)
    m_reference->RegisterActuator(this);
  UpdateFuzzyFlags();
}

SCA_ObjectActuator::~SCA_ObjectActuator()
{
  if (m_reference)
    m_reference->UnregisterActuator(this);
}

bool SCA_ObjectActuator::Update()
{

  bool bNegativeEvent = IsNegativeEvent();
  RemoveAllEvents();

  KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());
  PHY_ICharacter *character = parent->GetScene()->GetPhysicsEnvironment()->GetCharacterController(
      parent);

  if (bNegativeEvent) {
    // Explicitly stop the movement if we're using character motion
    if (m_bitLocalFlag.CharacterMotion) {
      character->SetWalkDirection(MT_Vector3(0.0f, 0.0f, 0.0f));
    }

    m_linear_damping_active = false;
    m_angular_damping_active = false;
    m_error_accumulator.setValue(0.0f, 0.0f, 0.0f);
    m_previous_error.setValue(0.0f, 0.0f, 0.0f);
    m_jumping = false;
    return false;
  }
  else if (parent) {
    if (m_bitLocalFlag.ServoControl) {
      // In this mode, we try to reach a target speed using force
      // As we don't know the friction, we must implement a generic
      // servo control to achieve the speed in a configurable
      // v = current velocity
      // V = target velocity
      // e = V-v = speed error
      // dt = time interval since previous update
      // I = sum(e(t)*dt)
      // dv = e(t) - e(t-1)
      // KP, KD, KI : coefficient
      // F = KP*e+KI*I+KD*dv
      MT_Scalar mass = parent->GetMass();
      if (mass < MT_EPSILON) {
        return false;
      }
      MT_Vector3 v;
      if (m_bitLocalFlag.ServoControlAngular) {
        v = parent->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
      }
      else {
        v = parent->GetLinearVelocity(m_bitLocalFlag.LinearVelocity);
      }
      if (m_reference) {
        if (m_bitLocalFlag.ServoControlAngular) {
          const MT_Vector3 vel = m_reference->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
          v -= vel;
        }
        else {
          const MT_Vector3& mypos = parent->NodeGetWorldPosition();
          const MT_Vector3& refpos = m_reference->NodeGetWorldPosition();
          const MT_Vector3 relpos = (mypos - refpos);
          MT_Vector3 vel = m_reference->GetVelocity(relpos);
          if (m_bitLocalFlag.LinearVelocity) {
            // must convert in local space
            vel = parent->NodeGetWorldOrientation().transposed() * vel;
          }
          v -= vel;
        }
      }

      MT_Vector3 e;
      if (m_bitLocalFlag.ServoControlAngular) {
        e = m_angular_velocity - v;
      }
      else {
        e = m_linear_velocity - v;
      }

      MT_Vector3 dv = e - m_previous_error;
      MT_Vector3 I = m_error_accumulator + e;

      MT_Vector3& f = (m_bitLocalFlag.ServoControlAngular) ? m_force : m_torque;
      f = m_pid.x() * e + m_pid.y() * I + m_pid.z() * dv;

      /* Make sure velocity is correct depending on how body react to force/torque.
       * See btRigidBody::integrateVelocities */
      if (m_bitLocalFlag.ServoControlAngular) {
        f = f * parent->GetLocalInertia();
      }
      else {
        f *= mass;
      }

      const bool limits[3] = {m_bitLocalFlag.Torque, m_bitLocalFlag.DLoc, m_bitLocalFlag.DRot};

      for (unsigned short i = 0; i <  3; ++i) {
        if (!limits[i]) {
          continue;
        }
        if (f[i] > m_dloc[i]) {
          f[i] = m_dloc[i];
          I[i] = m_error_accumulator[i];
        }
        else if (f[i] < m_drot[i]) {
          f[i] = m_drot[i];
          I[i] = m_error_accumulator[i];
        }
      }
      m_previous_error = e;
      m_error_accumulator = I;
      if (m_bitLocalFlag.ServoControlAngular) {
        parent->ApplyTorque(f, m_bitLocalFlag.AngularVelocity);
      }
      else {
        parent->ApplyForce(f, m_bitLocalFlag.LinearVelocity);
      }
    }
    else if (m_bitLocalFlag.CharacterMotion) {
      MT_Vector3 dir = m_dloc;

      if (m_bitLocalFlag.DLoc) {
        MT_Matrix3x3 basis = parent->GetPhysicsController()->GetOrientation();
        dir = basis * dir;
      }

      if (m_bitLocalFlag.AddOrSetCharLoc) {
        MT_Vector3 old_dir = character->GetWalkDirection();

        if (!old_dir.fuzzyZero()) {
          MT_Scalar mag = old_dir.length();

          dir = dir + old_dir;
          if (!dir.fuzzyZero())
            dir = dir.normalized() * mag;
        }
      }

      // We always want to set the walk direction since a walk direction of (0, 0, 0) should stop
      // the character
      character->SetWalkDirection(
          dir / parent->GetScene()->GetPhysicsEnvironment()->GetNumTimeSubSteps());

      if (!m_bitLocalFlag.ZeroDRot) {
        parent->ApplyRotation(m_drot, (m_bitLocalFlag.DRot) != 0);
      }

      if (m_bitLocalFlag.CharacterJump) {
        if (!m_jumping) {
          character->Jump();
          m_jumping = true;
        }
        else if (character->OnGround())
          m_jumping = false;
      }
    }
    else {
      if (!m_bitLocalFlag.ZeroForce) {
        parent->ApplyForce(m_force, (m_bitLocalFlag.Force) != 0);
      }
      if (!m_bitLocalFlag.ZeroTorque) {
        parent->ApplyTorque(m_torque, (m_bitLocalFlag.Torque) != 0);
      }
      if (!m_bitLocalFlag.ZeroDLoc) {
        parent->ApplyMovement(m_dloc, (m_bitLocalFlag.DLoc) != 0);
      }
      if (!m_bitLocalFlag.ZeroDRot) {
        parent->ApplyRotation(m_drot, (m_bitLocalFlag.DRot) != 0);
      }
      if (!m_bitLocalFlag.ZeroLinearVelocity) {
        if (m_bitLocalFlag.AddOrSetLinV) {
          parent->addLinearVelocity(m_linear_velocity, (m_bitLocalFlag.LinearVelocity) != 0);
        }
        else {
          if (m_damping > 0) {
            MT_Vector3 linV;
            if (!m_linear_damping_active) {
              // delta and the start speed (depends on the existing speed in that direction)
              linV = parent->GetLinearVelocity(m_bitLocalFlag.LinearVelocity);
              // keep only the projection along the desired direction
              m_current_linear_factor = linV.dot(m_linear_velocity) / m_linear_length2;
              m_linear_damping_active = true;
            }
            if (m_current_linear_factor < 1.0f)
              m_current_linear_factor += 1.0f / m_damping;
            if (m_current_linear_factor > 1.0f)
              m_current_linear_factor = 1.0f;
            linV = m_current_linear_factor * m_linear_velocity;
            parent->setLinearVelocity(linV, (m_bitLocalFlag.LinearVelocity) != 0);
          }
          else {
            parent->setLinearVelocity(m_linear_velocity, (m_bitLocalFlag.LinearVelocity) != 0);
          }
        }
      }
      if (!m_bitLocalFlag.ZeroAngularVelocity) {
        if (m_damping > 0) {
          MT_Vector3 angV;
          if (!m_angular_damping_active) {
            // delta and the start speed (depends on the existing speed in that direction)
            angV = parent->GetAngularVelocity(m_bitLocalFlag.AngularVelocity);
            // keep only the projection along the desired direction
            m_current_angular_factor = angV.dot(m_angular_velocity) / m_angular_length2;
            m_angular_damping_active = true;
          }
          if (m_current_angular_factor < 1.0)
            m_current_angular_factor += 1.0 / m_damping;
          if (m_current_angular_factor > 1.0)
            m_current_angular_factor = 1.0;
          angV = m_current_angular_factor * m_angular_velocity;
          parent->setAngularVelocity(angV, (m_bitLocalFlag.AngularVelocity) != 0);
        }
        else {
          parent->setAngularVelocity(m_angular_velocity, (m_bitLocalFlag.AngularVelocity) != 0);
        }
      }
    }
  }
  return true;
}

EXP_Value *SCA_ObjectActuator::GetReplica()
{
  SCA_ObjectActuator *replica = new SCA_ObjectActuator(*this);  // m_float,GetName());
  replica->ProcessReplica();

  return replica;
}

void SCA_ObjectActuator::ProcessReplica()
{
  SCA_IActuator::ProcessReplica();
  if (m_reference)
    m_reference->RegisterActuator(this);
}

bool SCA_ObjectActuator::UnlinkObject(SCA_IObject *clientobj)
{
  if (clientobj == (SCA_IObject *)m_reference) {
    // this object is being deleted, we cannot continue to use it as reference.
    m_reference = nullptr;
    return true;
  }
  return false;
}

void SCA_ObjectActuator::Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map)
{
  KX_GameObject *obj = static_cast<KX_GameObject *>(obj_map[m_reference]);
  if (obj) {
    if (m_reference)
      m_reference->UnregisterActuator(this);
    m_reference = obj;
    m_reference->RegisterActuator(this);
  }
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_ObjectActuator::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_ObjectActuator",
                                         sizeof(EXP_PyObjectPlus_Proxy),
                                         0,
                                         py_base_dealloc,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_repr,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         Methods,
                                         0,
                                         0,
                                         &SCA_IActuator::Type,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         py_base_new};

PyMethodDef SCA_ObjectActuator::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_ObjectActuator::Attributes[] = {
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "force", -1000, 1000, false, SCA_ObjectActuator, m_force, PyUpdateFuzzyFlags),
    EXP_PYATTRIBUTE_BOOL_RW("useLocalForce", SCA_ObjectActuator, m_bitLocalFlag.Force),
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "torque", -1000, 1000, false, SCA_ObjectActuator, m_torque, PyUpdateFuzzyFlags),
    EXP_PYATTRIBUTE_BOOL_RW("useLocalTorque", SCA_ObjectActuator, m_bitLocalFlag.Torque),
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "dLoc", -1000, 1000, false, SCA_ObjectActuator, m_dloc, PyUpdateFuzzyFlags),
    EXP_PYATTRIBUTE_BOOL_RW("useLocalDLoc", SCA_ObjectActuator, m_bitLocalFlag.DLoc),
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "dRot", -1000, 1000, false, SCA_ObjectActuator, m_drot, PyUpdateFuzzyFlags),
    EXP_PYATTRIBUTE_BOOL_RW("useLocalDRot", SCA_ObjectActuator, m_bitLocalFlag.DRot),
#  ifdef USE_MATHUTILS
    EXP_PYATTRIBUTE_RW_FUNCTION("linV", SCA_ObjectActuator, pyattr_get_linV, pyattr_set_linV),
    EXP_PYATTRIBUTE_RW_FUNCTION("angV", SCA_ObjectActuator, pyattr_get_angV, pyattr_set_angV),
#  else
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "linV", -1000, 1000, false, SCA_ObjectActuator, m_linear_velocity, PyUpdateFuzzyFlags),
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK(
        "angV", -1000, 1000, false, SCA_ObjectActuator, m_angular_velocity, PyUpdateFuzzyFlags),
#  endif
    EXP_PYATTRIBUTE_BOOL_RW("useLocalLinV", SCA_ObjectActuator, m_bitLocalFlag.LinearVelocity),
    EXP_PYATTRIBUTE_BOOL_RW("useLocalAngV", SCA_ObjectActuator, m_bitLocalFlag.AngularVelocity),
    EXP_PYATTRIBUTE_SHORT_RW("damping", 0, 1000, false, SCA_ObjectActuator, m_damping),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "forceLimitX", SCA_ObjectActuator, pyattr_get_forceLimitX, pyattr_set_forceLimitX),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "forceLimitY", SCA_ObjectActuator, pyattr_get_forceLimitY, pyattr_set_forceLimitY),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "forceLimitZ", SCA_ObjectActuator, pyattr_get_forceLimitZ, pyattr_set_forceLimitZ),
    EXP_PYATTRIBUTE_VECTOR_RW_CHECK("pid", -100, 200, true, SCA_ObjectActuator, m_pid, PyCheckPid),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "reference", SCA_ObjectActuator, pyattr_get_reference, pyattr_set_reference),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/* Attribute get/set functions */

#  ifdef USE_MATHUTILS

/* These require an SGNode */
#    define MATHUTILS_VEC_CB_LINV 1
#    define MATHUTILS_VEC_CB_ANGV 2

static unsigned char mathutils_kxobactu_vector_cb_index = -1; /* index for our callbacks */

static int mathutils_obactu_generic_check(BaseMathObject *bmo)
{
  SCA_ObjectActuator *self = static_cast<SCA_ObjectActuator *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  return 0;
}

static int mathutils_obactu_vector_get(BaseMathObject *bmo, int subtype)
{
  SCA_ObjectActuator *self = static_cast<SCA_ObjectActuator *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  switch (subtype) {
    case MATHUTILS_VEC_CB_LINV:
      self->m_linear_velocity.getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_ANGV:
      self->m_angular_velocity.getValue(bmo->data);
      break;
  }

  return 0;
}

static int mathutils_obactu_vector_set(BaseMathObject *bmo, int subtype)
{
  SCA_ObjectActuator *self = static_cast<SCA_ObjectActuator *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  switch (subtype) {
    case MATHUTILS_VEC_CB_LINV:
      self->m_linear_velocity.setValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_ANGV:
      self->m_angular_velocity.setValue(bmo->data);
      break;
  }

  return 0;
}

static int mathutils_obactu_vector_get_index(BaseMathObject *bmo, int subtype, int index)
{
  /* lazy, avoid repeteing the case statement */
  if (mathutils_obactu_vector_get(bmo, subtype) == -1)
    return -1;
  return 0;
}

static int mathutils_obactu_vector_set_index(BaseMathObject *bmo, int subtype, int index)
{
  float f = bmo->data[index];

  /* lazy, avoid repeteing the case statement */
  if (mathutils_obactu_vector_get(bmo, subtype) == -1)
    return -1;

  bmo->data[index] = f;
  return mathutils_obactu_vector_set(bmo, subtype);
}

static Mathutils_Callback mathutils_obactu_vector_cb = {mathutils_obactu_generic_check,
                                                        mathutils_obactu_vector_get,
                                                        mathutils_obactu_vector_set,
                                                        mathutils_obactu_vector_get_index,
                                                        mathutils_obactu_vector_set_index};

PyObject *SCA_ObjectActuator::pyattr_get_linV(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxobactu_vector_cb_index,
                                  MATHUTILS_VEC_CB_LINV);
}

int SCA_ObjectActuator::pyattr_set_linV(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  SCA_ObjectActuator *self = static_cast<SCA_ObjectActuator *>(self_v);
  if (!PyVecTo(value, self->m_linear_velocity))
    return PY_SET_ATTR_FAIL;

  self->UpdateFuzzyFlags();

  return PY_SET_ATTR_SUCCESS;
}

PyObject *SCA_ObjectActuator::pyattr_get_angV(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxobactu_vector_cb_index,
                                  MATHUTILS_VEC_CB_ANGV);
}

int SCA_ObjectActuator::pyattr_set_angV(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value)
{
  SCA_ObjectActuator *self = static_cast<SCA_ObjectActuator *>(self_v);
  if (!PyVecTo(value, self->m_angular_velocity))
    return PY_SET_ATTR_FAIL;

  self->UpdateFuzzyFlags();

  return PY_SET_ATTR_SUCCESS;
}

void SCA_ObjectActuator_Mathutils_Callback_Init(void)
{
  // register mathutils callbacks, ok to run more than once.
  mathutils_kxobactu_vector_cb_index = Mathutils_RegisterCallback(&mathutils_obactu_vector_cb);
}

#  endif  // USE_MATHUTILS

PyObject *SCA_ObjectActuator::pyattr_get_forceLimitX(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);
  PyObject *retVal = PyList_New(3);

  PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[0]));
  PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[0]));
  PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.Torque));

  return retVal;
}

int SCA_ObjectActuator::pyattr_set_forceLimitX(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);

  PyObject *seq = PySequence_Fast(value, "");
  if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
    self->m_drot[0] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
    self->m_dloc[0] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
    self->m_bitLocalFlag.Torque = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

    if (!PyErr_Occurred()) {
      Py_DECREF(seq);
      return PY_SET_ATTR_SUCCESS;
    }
  }

  Py_XDECREF(seq);

  PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
  return PY_SET_ATTR_FAIL;
}

PyObject *SCA_ObjectActuator::pyattr_get_forceLimitY(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);
  PyObject *retVal = PyList_New(3);

  PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[1]));
  PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[1]));
  PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.DLoc));

  return retVal;
}

int SCA_ObjectActuator::pyattr_set_forceLimitY(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);

  PyObject *seq = PySequence_Fast(value, "");
  if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
    self->m_drot[1] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
    self->m_dloc[1] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
    self->m_bitLocalFlag.DLoc = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

    if (!PyErr_Occurred()) {
      Py_DECREF(seq);
      return PY_SET_ATTR_SUCCESS;
    }
  }

  Py_XDECREF(seq);

  PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
  return PY_SET_ATTR_FAIL;
}

PyObject *SCA_ObjectActuator::pyattr_get_forceLimitZ(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);
  PyObject *retVal = PyList_New(3);

  PyList_SET_ITEM(retVal, 0, PyFloat_FromDouble(self->m_drot[2]));
  PyList_SET_ITEM(retVal, 1, PyFloat_FromDouble(self->m_dloc[2]));
  PyList_SET_ITEM(retVal, 2, PyBool_FromLong(self->m_bitLocalFlag.DRot));

  return retVal;
}

int SCA_ObjectActuator::pyattr_set_forceLimitZ(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  SCA_ObjectActuator *self = reinterpret_cast<SCA_ObjectActuator *>(self_v);

  PyObject *seq = PySequence_Fast(value, "");
  if (seq && PySequence_Fast_GET_SIZE(seq) == 3) {
    self->m_drot[2] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 0));
    self->m_dloc[2] = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(value, 1));
    self->m_bitLocalFlag.DRot = (PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 2)) != 0);

    if (!PyErr_Occurred()) {
      Py_DECREF(seq);
      return PY_SET_ATTR_SUCCESS;
    }
  }

  Py_XDECREF(seq);

  PyErr_SetString(PyExc_ValueError, "expected a sequence of 2 floats and a bool");
  return PY_SET_ATTR_FAIL;
}

PyObject *SCA_ObjectActuator::pyattr_get_reference(EXP_PyObjectPlus *self,
                                                   const struct EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_ObjectActuator *actuator = static_cast<SCA_ObjectActuator *>(self);
  if (!actuator->m_reference)
    Py_RETURN_NONE;

  return actuator->m_reference->GetProxy();
}

int SCA_ObjectActuator::pyattr_set_reference(EXP_PyObjectPlus *self,
                                             const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  SCA_ObjectActuator *actuator = static_cast<SCA_ObjectActuator *>(self);
  KX_GameObject *refOb;

  if (!ConvertPythonToGameObject(actuator->GetLogicManager(),
                                 value,
                                 &refOb,
                                 true,
                                 "actu.reference = value: SCA_ObjectActuator"))
    return PY_SET_ATTR_FAIL;

  if (actuator->m_reference)
    actuator->m_reference->UnregisterActuator(actuator);

  if (refOb == nullptr) {
    actuator->m_reference = nullptr;
  }
  else {
    actuator->m_reference = refOb;
    actuator->m_reference->RegisterActuator(actuator);
  }

  return PY_SET_ATTR_SUCCESS;
}

#endif  // WITH_PYTHON

/* eof */

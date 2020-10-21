// Copyright (C) 2019 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine" and was originally part of the "Irrlicht Engine"
// For conditions of distribution and use, see copyright notice in nabla.h
// See the original file in irrlicht source for authors

#ifndef __NBL_I_SCENE_NODE_ANIMATOR_CAMERA_FPS_H_INCLUDED__
#define __NBL_I_SCENE_NODE_ANIMATOR_CAMERA_FPS_H_INCLUDED__

#include "ISceneNodeAnimator.h"
#include "IEventReceiver.h"

#include "SKeyMap.h"

namespace irr
{

namespace scene
{

	//! Special scene node animator for FPS cameras
	/** This scene node animator can be attached to a camera to make it act
	like a first person shooter
	*/
	class ISceneNodeAnimatorCameraFPS : public ISceneNodeAnimator
	{
	public:

		//! Returns the speed of movement in units per millisecond
		virtual float getMoveSpeed() const = 0;

		//! Sets the speed of movement in units per millisecond
		virtual void setMoveSpeed(float moveSpeed) = 0;

		//! Returns the rotation speed in degrees
		/** The degrees are equivalent to a half screen movement of the mouse,
		i.e. if the mouse cursor had been moved to the border of the screen since
		the last animation. */
		virtual float getRotateSpeed() const = 0;

		//! Set the rotation speed in degrees
		virtual void setRotateSpeed(float rotateSpeed) = 0;

		//! Sets the keyboard mapping for this animator (old style)
		/** \param map Array of keyboard mappings, see irr::SKeyMap
		\param count Size of the keyboard map array. */
		virtual void setKeyMap(SKeyMap *map, uint32_t count) = 0;

		//! Sets the keyboard mapping for this animator
		//!	\param keymap The new keymap array
		virtual void setKeyMap(const core::vector<SKeyMap>& keymap) = 0;

		//! Gets the keyboard mapping for this animator
		virtual const core::vector<SKeyMap>& getKeyMap() const = 0;

		//! Sets whether vertical movement should be allowed.
		/** If vertical movement is enabled then the camera may fight with
		gravity causing camera shake. Disable this if the camera has
		a collision animator with gravity enabled. */
		virtual void setVerticalMovement(bool allow) = 0;

		//! Sets whether the Y axis of the mouse should be inverted.
		/** If enabled then moving the mouse down will cause
		the camera to look up. It is disabled by default. */
		virtual void setInvertMouse(bool invert) = 0;
	};
} // end namespace scene
} // end namespace irr

#endif


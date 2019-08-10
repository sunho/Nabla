﻿#ifndef __C_ELEMENT_SHAPE_OBJ_H_INCLUDED__
#define __C_ELEMENT_SHAPE_OBJ_H_INCLUDED__

#include "../../ext/MitsubaLoader/IElement.h"
#include "../../ext/MitsubaLoader/IShape.h"
#include "irrlicht.h"

namespace irr { namespace ext { namespace MitsubaLoader {

class CElementShapeOBJ : public IElement, public IShape
{
public:
	CElementShapeOBJ()
		:faceNormals(false),
		smoothNormals(false),
		maxSmoothAngle(0.0f),
		flipTexCoords(true),
		collapse(false) {};

	~CElementShapeOBJ();

	virtual bool processAttributes(const char** _args) override;
	virtual bool onEndTag(asset::IAssetManager& _assetManager, IElement* _parent) override;
	virtual IElement::Type getType() const override { return IElement::Type::SHAPE;  };
	virtual std::string getLogName() const override { return "shape_obj"; };
	virtual bool processChildData(IElement* child) override;

private:
	std::string fileName;
	bool faceNormals;
	//indicates if normals should be smoothed (false by default)
	bool smoothNormals;
	float maxSmoothAngle;
	bool flipTexCoords;
	bool collapse; //?
};

}
}
}

#endif
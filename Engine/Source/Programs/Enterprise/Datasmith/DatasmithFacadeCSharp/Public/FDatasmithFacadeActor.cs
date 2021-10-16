// Copyright Epic Games, Inc. All Rights Reserved.

//------------------------------------------------------------------------------
// <auto-generated />
//
// This file was automatically generated by SWIG (http://www.swig.org).
// Version 4.0.1
//
// Do not make changes to this file unless you know what you are doing--modify
// the SWIG interface file instead.
//------------------------------------------------------------------------------


public class FDatasmithFacadeActor : FDatasmithFacadeElement {
  private global::System.Runtime.InteropServices.HandleRef swigCPtr;

  internal FDatasmithFacadeActor(global::System.IntPtr cPtr, bool cMemoryOwn) : base(DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SWIGUpcast(cPtr), cMemoryOwn) {
    swigCPtr = new global::System.Runtime.InteropServices.HandleRef(this, cPtr);
  }

  internal static global::System.Runtime.InteropServices.HandleRef getCPtr(FDatasmithFacadeActor obj) {
    return (obj == null) ? new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero) : obj.swigCPtr;
  }

  protected override void Dispose(bool disposing) {
    lock(this) {
      if (swigCPtr.Handle != global::System.IntPtr.Zero) {
        if (swigCMemOwn) {
          swigCMemOwn = false;
          DatasmithFacadeCSharpPINVOKE.delete_FDatasmithFacadeActor(swigCPtr);
        }
        swigCPtr = new global::System.Runtime.InteropServices.HandleRef(null, global::System.IntPtr.Zero);
      }
      base.Dispose(disposing);
    }
  }

  public FDatasmithFacadeActor(string InElementName) : this(DatasmithFacadeCSharpPINVOKE.new_FDatasmithFacadeActor(InElementName), true) {
  }

  public void SetWorldTransform(float[] InWorldMatrix, bool bRowMajor) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetWorldTransform__SWIG_0(swigCPtr, InWorldMatrix, bRowMajor);
  }

  public void SetWorldTransform(float[] InWorldMatrix) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetWorldTransform__SWIG_1(swigCPtr, InWorldMatrix);
  }

  public void SetScale(float X, float Y, float Z) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetScale(swigCPtr, X, Y, Z);
  }

  public void GetScale(out float OutX, out float OutY, out float OutZ) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetScale(swigCPtr, out OutX, out OutY, out OutZ);
  }

  public void SetRotation(float Pitch, float Yaw, float Roll) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetRotation__SWIG_0(swigCPtr, Pitch, Yaw, Roll);
  }

  public void GetRotation(out float OutPitch, out float OutYaw, out float OutRoll) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetRotation__SWIG_0(swigCPtr, out OutPitch, out OutYaw, out OutRoll);
  }

  public void SetRotation(float X, float Y, float Z, float W) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetRotation__SWIG_1(swigCPtr, X, Y, Z, W);
  }

  public void GetRotation(out float OutX, out float OutY, out float OutZ, out float OutW) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetRotation__SWIG_1(swigCPtr, out OutX, out OutY, out OutZ, out OutW);
  }

  public void SetTranslation(float X, float Y, float Z) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetTranslation(swigCPtr, X, Y, Z);
  }

  public void GetTranslation(out float OutX, out float OutY, out float OutZ) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetTranslation(swigCPtr, out OutX, out OutY, out OutZ);
  }

  public void SetLayer(string InLayerName) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetLayer(swigCPtr, InLayerName);
  }

  public string GetLayer() {
    string ret = global::System.Runtime.InteropServices.Marshal.PtrToStringUni(DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetLayer(swigCPtr));
    return ret;
  }

  public void AddTag(string InTag) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_AddTag(swigCPtr, InTag);
  }

  public void ResetTags() {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_ResetTags(swigCPtr);
  }

  public int GetTagsCount() {
    int ret = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetTagsCount(swigCPtr);
    return ret;
  }

  public string GetTag(int TagIndex) {
    string ret = global::System.Runtime.InteropServices.Marshal.PtrToStringUni(DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetTag(swigCPtr, TagIndex));
    return ret;
  }

  public bool IsComponent() {
    bool ret = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_IsComponent(swigCPtr);
    return ret;
  }

  public void SetIsComponent(bool bInIsComponent) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetIsComponent(swigCPtr, bInIsComponent);
  }

  public void AddChild(FDatasmithFacadeActor InChildActorPtr) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_AddChild(swigCPtr, FDatasmithFacadeActor.getCPtr(InChildActorPtr));
  }

  public int GetChildrenCount() {
    int ret = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetChildrenCount(swigCPtr);
    return ret;
  }

  public FDatasmithFacadeActor GetChild(int InIndex) {
	global::System.IntPtr objectPtr = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetChild(swigCPtr, InIndex);
	if(objectPtr == global::System.IntPtr.Zero)
	{
		return null;
	}
	else
	{
		FDatasmithFacadeActor.EActorType ActorType = (new FDatasmithFacadeActor(objectPtr, false)).GetActorType();

		switch(ActorType)
		{
		case FDatasmithFacadeActor.EActorType.DirectionalLight:
			return new FDatasmithFacadeDirectionalLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.AreaLight:
			return new FDatasmithFacadeAreaLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.LightmassPortal:
			return new FDatasmithFacadeLightmassPortal(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.PointLight:
			return new FDatasmithFacadePointLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.SpotLight:
			return new FDatasmithFacadeSpotLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.StaticMeshActor:
			return new FDatasmithFacadeActorMesh(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Camera:
			return new FDatasmithFacadeActorCamera(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Actor:
			return new FDatasmithFacadeActor(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Unsupported:
		default:
			return null;
		}
	}
}

  public void RemoveChild(FDatasmithFacadeActor InChild) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_RemoveChild(swigCPtr, FDatasmithFacadeActor.getCPtr(InChild));
  }

  public FDatasmithFacadeActor GetParentActor() {
	global::System.IntPtr objectPtr = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetParentActor(swigCPtr);
	if(objectPtr == global::System.IntPtr.Zero)
	{
		return null;
	}
	else
	{
		FDatasmithFacadeActor.EActorType ActorType = (new FDatasmithFacadeActor(objectPtr, false)).GetActorType();

		switch(ActorType)
		{
		case FDatasmithFacadeActor.EActorType.DirectionalLight:
			return new FDatasmithFacadeDirectionalLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.AreaLight:
			return new FDatasmithFacadeAreaLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.LightmassPortal:
			return new FDatasmithFacadeLightmassPortal(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.PointLight:
			return new FDatasmithFacadePointLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.SpotLight:
			return new FDatasmithFacadeSpotLight(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.StaticMeshActor:
			return new FDatasmithFacadeActorMesh(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Camera:
			return new FDatasmithFacadeActorCamera(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Actor:
			return new FDatasmithFacadeActor(objectPtr, true);
		case FDatasmithFacadeActor.EActorType.Unsupported:
		default:
			return null;
		}
	}
}

  public void SetVisibility(bool bInVisibility) {
    DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_SetVisibility(swigCPtr, bInVisibility);
  }

  public bool GetVisibility() {
    bool ret = DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetVisibility(swigCPtr);
    return ret;
  }

  public FDatasmithFacadeActor.EActorType GetActorType() {
    FDatasmithFacadeActor.EActorType ret = (FDatasmithFacadeActor.EActorType)DatasmithFacadeCSharpPINVOKE.FDatasmithFacadeActor_GetActorType(swigCPtr);
    return ret;
  }

  public enum EActorType {
    DirectionalLight,
    AreaLight,
    EnvironmentLight,
    LightmassPortal,
    PointLight,
    SpotLight,
    StaticMeshActor,
    Camera,
    Actor,
    Unsupported
  }

}

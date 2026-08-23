local ParticleCount = 128;
{
    "fx": {
        "cereal_class_version": 0,
		"category" : "postprocess",
        "controllers": [
			{ "name":"CenterMatrix", "type":"float4x4", "controllerName":"(self)", "item":"センター",
                "desc":["Specifies the position and direction of the firing source","発射元の位置と発射方向を指定します"] },
			{ "name":"AimBone", "type":"float3", "controllerName":"(self)", "item":"狙い",
                "desc":["If the aiming morph is greater than 0, it fires in the direction of the aiming bone.","狙い有効モーフが0より大きい時、狙いボーンの方向へ発射します"] },
			{ "name":"Aim", "type":"float", "controllerName":"(self)", "item":"狙い有効",
                "desc":["If the aiming morph is greater than 0, it fires in the direction of the aiming bone.","狙い有効モーフが0より大きい時、狙いボーンの方向へ発射します"] },
			{ "name":"InjectionBone", "type":"float3", "controllerName":"(self)", "item":"噴射", "desc":["",""] },
			{ "name":"_Amount", "type":"float", "controllerName":"(self)", "item":"少なく",
                "desc":["Reduces the amount of particles displayed.","表示量を少なくします"] },
			{ "name":"_AnimSpeed", "type":"float", "controllerName":"(self)", "item":"速度",
                "desc":["the speed of the animation","アニメーションのスピードを指定します"] },
			{ "name":"_Arc", "type":"float", "controllerName":"(self)", "item":"広がり",
                "desc":["the variation of the jet angle","噴射角度のバラつきを指定します"] },
			{ "name":"_Koshi", "type":"float", "controllerName":"(self)", "item":"コシ",
                "desc":["The light and dark will be more vivid","明暗にメリハリが出ます"] },
			{ "name":"_EntireSize", "type":"float", "controllerName":"(self)", "item":"サイズ",
                "desc":["the overall scale","全体的なスケールを指定します"] },
			{ "name":"_A", "type":"float", "controllerName":"(self)", "item":"透明度", "desc":["",""] },
			{ "name":"H", "type":"float", "controllerName":"(self)", "item":"H", "desc":["Hue","色相"] },
			{ "name":"S", "type":"float", "controllerName":"(self)", "item":"S", "desc":["Saturation","彩度"] },
			{ "name":"V", "type":"float", "controllerName":"(self)", "item":"V", "desc":["Value","明度"] },
			{ "name":"_Intensity", "type":"float", "controllerName":"(self)", "item":"明るく", "desc":["",""] },
		],
		"samplers":[
			{ "name":"samp" }
		],
		"buffers":[
			{ "name":"IBuf", "type":"uint", "size":{"absolute":true, "width":ParticleCount*6}, "view":"UAV" },
		],
		"textures":[
		],
        "passes": [
			{ "name":"Copy", "type":"postprocess", "pixelShader":"PS", "vertexShader":"VS" },
			{
				"name":"Computer", "type":"compute", "computeShader":"CS", "UAV":[{"name":"IBuf"}],
				"outputSize":{"absolute":true, "dimension":1, "width":ParticleCount, "conditions":["OnStart"]}
			},
		]
    }
}
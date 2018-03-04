<?php
# Enable display of all errors
error_reporting(E_ALL); 
ini_set('display_errors', 1);

$fconfig = 'config';
$config = "host=localhost user=postgres";
if((file_exists($fconfig)) && (is_readable($fconfig))) {
  $config = file_get_contents($fconfig);  
} else {
  echo 'File '.$fconfig.' doesn\'t exist.';
}

function getdb($c) {
  # Method 'trust' avoid to use password. Config in /etc/postgresql/10.6/main/pg_hba.conf
  $db = pg_connect($c) or die('connection failed');
  return $db;
}

$db = getdb($config);
?>

<!doctype html>
<html>
  <head>
    <title>Where Panel</title>

    <!-- Required meta tags -->
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, shrink-to-fit=no">

    <!-- Bootstrap CSS -->
    <link rel="stylesheet" href="https://maxcdn.bootstrapcdn.com/bootstrap/4.0.0/css/bootstrap.min.css" integrity="sha384-Gn5384xqQ1aoWXA+058RXPxPg6fy4IWvTNh0E263XmFcJlSAwiGgFAW/dAiS6JXm" crossorigin="anonymous">

    <script>
      function mouseOver(str) {
        var dec = str.split(" ");
        var wpc = dec[1].substring(1,dec[1].length-1).split(",");
        var ids = wpc[dec[0]-1].substring(1,wpc[dec[0]-1].length-1).split(";");
        for(var i = 0; i < ids.length; i++) {
          document.getElementById(ids[i]).style.color = "red";
        }
      }

      function mouseOut(str) {
        var dec = str.split(" ");
        var wpc = dec[1].substring(1,dec[1].length-1).split(",");
        var ids = wpc[dec[0]-1].substring(1,wpc[dec[0]-1].length-1).split(";");
        for(var i = 0; i < ids.length; i++) {
          document.getElementById(ids[i]).style.color = "";
        }
      }

      function exemple(str) {
        document.getElementById('request').value = str;
      }
    </script>
  </head>
  <body>
    <nav class="navbar navbar-expand navbar-dark bg-dark">
      <a class="navbar-brand" href="#">Where Provenance</a>
      <button class="navbar-toggler" type="button" data-toggle="collapse" data-target="#navbarsExample02" aria-controls="navbarsExample02" aria-expanded="false" aria-label="Toggle navigation">
        <span class="navbar-toggler-icon"></span>
      </button>

      <!-- <div class="collapse navbar-collapse" id="navbarsExample02">
        <ul class="navbar-nav mr-auto">
          <li class="nav-item active">
            <a class="nav-link" href="#">Home <span class="sr-only">(current)</span></a>
          </li>
          <li class="nav-item">
            <a class="nav-link" href="config">Config</a>
          </li>
        </ul>
      </div> -->
    </nav>

    <!-- <div class="jumbotron">
      <div class="container">
        <h1 class="display-3">Where Provenance</h1>
        <p>Query easily your PostgreSQL database and check the provenance of your result.</p>
        <p><a class="btn btn-primary btn-lg" href="config" role="button">Edit config file &raquo;</a></p>
      </div>
    </div> -->

    <!--<div id="conteneur">
      <div id="left-panel">-->
    <div class="container-fluid d-md-flex flex-md-equal w-100 my-md-3 pl-md-3 ">
      <div class="bg-light mr-md-3 pt-3 px-3 pt-md-5 px-md-5 text-center text-black overflow-hidden col-md-5">

      <div class="my-3 py-3">
        <h2 class="display-5">Your Database</h2>
        <p class="lead">Overview of provenance-tagged relations</p>
      </div>

    <?php
    # PHP CODE FOR LEFT PANEL:
    #  list and print all tables tagged with provenance  
    $req = "
      SELECT relname, 
             a1.attrelid,
             (SELECT count(*) FROM pg_attribute a2 WHERE a2.attrelid=a1.attrelid AND attnum>0)-1 c
      FROM pg_attribute a1 	JOIN pg_type 		ON atttypid=pg_type.oid
                           	JOIN pg_namespace ns1 	ON typnamespace=ns1.oid
                           	JOIN pg_class 		ON attrelid=pg_class.oid
                           	JOIN pg_namespace ns2 	ON relnamespace=ns2.oid
      WHERE typname='provenance_token' 
        AND relkind='r' 
        AND ns1.nspname='provsql' 
        AND ns2.nspname<>'provsql' 
        AND attname='provsql'";
    $r = pg_exec($db , $req); 
    for ($i=0; $i<pg_numrows($r); $i++) {
      $l=pg_fetch_array($r,$i);
      echo "<h2>".$l["relname"]."</h2>";
      echo "<table class='table table-bordered table-striped table-condensed'>";
      # ENTETES: affichage des noms de colonnes en balise <th>
      $reqh = " 
        SELECT attname
        FROM pg_attribute
	WHERE attrelid = ".$l["attrelid"]."
          AND attnum > 0
        ORDER BY attnum";
      echo "<tr>";
      $rh = pg_exec($db, $reqh);
      for ($ih=0; $ih<pg_numrows($rh)-1; $ih++) {
	$lh = pg_fetch_array($rh,$ih);
        echo "<th>".$lh["attname"]."</th>"; 
      }
      echo "</tr>";
      # CONTENU: affichage des données avec en balise <tr>
      $nil = pg_exec($db, "SET search_path to public,provsql");
      $r2 = pg_exec($db, "select * from (select * from ".$l["relname"]." ) t");
      for ($i2=0; $i2<pg_numrows($r2); $i2++) {
        $l2=pg_fetch_array($r2,$i2);
	echo '<tr title="'.$l2[$l["c"]].'">';
        for ($j=0; $j<$l["c"]; $j++) {
           echo '<td id="'.$l["relname"].':'.$l2[$l["c"]].':'.($j+1).'">'.$l2[$j].'</td>';
        }
        echo "</tr>";
      }
      echo "</table>";
    }
    # END PHP CODE FOR LEFT PANEL
    ?>
      </div>
      <!-- <div id="right-panel"> -->
      <div class="bg-light mr-md-3 pt-3 px-3 pt-md-5 px-md-5 overflow-hidden col-md-7">
        <div class="my-3 py-3 text-center">
          <h2 class="display-5">Your Query</h2>
          <p class="lead">Type your SQL query (without ending semicolon)</p>
        </div>
	<form method="post" action="index.php">
          <label for="but1">Example queries: </label>
          <button id="but1" type="button" class="btn btn-primary" onClick="exemple('SELECT * FROM personnel')">All</button>
          <button id="but2" type="button" class="btn btn-primary" onClick="exemple('SELECT distinct city FROM personnel')">Distinct</button>
          <button id="but3" type="button" class="btn btn-primary" onClick="exemple('SELECT city FROM personnel UNION SELECT \'5\' FROM personnel')">Union</button>
          <br/> <p> </p>
	  <textarea id="request" name="request" rows=2 class="form-control"><?php
              if($_POST) echo $_POST['request'];
              else echo 'SELECT distinct city FROM personnel'; 
            ?></textarea>
          <input type="submit" name="button" value=" Send query " class="form-control">
        </form>
        
        <?php
          # AFFICHAGE: résultats de la requête et where en surbrillance
          if($_POST) { 
            $nil = pg_exec($db, "SET search_path to public,provsql");
	    $_POST['request'] = str_replace(';', '', $_POST['request']);
            $requ = "select *, provsql.where_provenance(provsql.provenance()) from (".$_POST['request'].") t";
	    $ru = pg_exec($db, $requ);
	    if($ru) {
              echo '<hr>';
	      echo "<h2 class='text-center'> query result</h2>\n<table class='table table-bordered table-striped table-condensed text-center'>\n";
	      $nfs = pg_num_fields($ru);
	      echo "  <tr>\n";
	      for ($fn = 0; $fn < $nfs-2; $fn++) {
	        echo "    <th>";
	        $fieldname = pg_field_name($ru, $fn);
	        echo $fieldname;
	        echo "</th>\n";
	      }
	      echo "  </tr>\n";
              /*$l3=pg_fetch_result($ru,0);
	      var_dump($l3);
	      echo "<tr>";
	      for ($j=0; $j<sizeof($l3)/2-2; $j++) {
	        echo "<th>".array_keys($l3)[2*$j+1]."</th>";
	      }
	      echo "</tr>";*/

	      for ($i3=0; $i3<pg_numrows($ru); $i3++) {
                $l3=pg_fetch_row($ru,$i3);
	        echo '  <tr title="'.$l3[sizeof($l3)-1].'">';
	        echo "\n";
                for ($j=0; $j<sizeof($l3)-2; $j++) {
		  echo '    <td id="'.($j+1).' '.$l3[sizeof($l3)-2].'" onmouseover="mouseOver(this.id)" onmouseout="mouseOut(this.id)" >'.$l3[$j].'</td>';
                  echo "\n";
                }
	        echo "  </tr>\n";
              }
	      echo "</table>\n";
	    }
	  }
        ?>
      </div>
    </div>
  </body>
</html>
